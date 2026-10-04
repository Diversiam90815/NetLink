/*
  ==============================================================================
	Module:         ReliableLink
	Description:    Reliable, ordered message streams to one remote peer on top
					of datagrams (Data -> DataAck -> AckAck per seq)
  ==============================================================================
*/

#include "ReliableLink.h"

#include <algorithm>
#include <limits>
#include <random>
#include <utility>

#include "Channel/Fragmentation/FragmentationService.h"
#include "NetLinkLog.h"


namespace netlink::channel
{

namespace
{

// What may wait for a send pass: a remote that floods the link between two passes must not grow these without bound.
// Whatever is refused is simply acknowledged or confirmed with the next retransmission.
constexpr size_t MaxPendingDataAcks = 4 * WindowSize;
constexpr size_t MaxPendingAckAcks	= 4 * WindowSize;

// Doublings of the retransmission timeout: far more than it takes to reach any maximum timeout
constexpr int	 MaxBackoffSteps	= 16;

} // namespace


uint32_t makeStreamID(const uint32_t different)
{
	// Seeded with more than one word of entropy: a single one leaves only 2^32 possible sequences of IDs
	thread_local std::mt19937 generator = []
	{
		std::random_device device;
		std::seed_seq	   seed{device(), device(), device(), device(), device(), device(), device(), device()};
		return std::mt19937(seed);
	}();
	std::uniform_int_distribution<uint32_t> distribution(1, UINT32_MAX);

	uint32_t								id = 0;
	do
	{
		id = distribution(generator);
	} while (id == different);

	return id;
}


std::vector<uint8_t> OutgoingDatagram::bytes() const
{
	const auto			 payload = body();
	std::vector<uint8_t> datagram(headSize + payload.size());

	std::copy_n(head.begin(), headSize, datagram.begin());
	std::ranges::copy(payload, datagram.begin() + headSize);
	return datagram;
}


ReliableLink::ReliableLink(const ReliabilityConfig &config, const uint32_t localStreamID)
	: mConfig(config), mLocalStreamID(localStreamID != 0 ? localStreamID : makeStreamID()), mRtt(config.initialRto, config.minRto, config.maxRto),
	  mCongestionWindow(static_cast<double>(std::clamp(config.initialCongestionWindow, config.minCongestionWindow, config.maxCongestionWindow))),
	  mSlowStartThreshold(std::numeric_limits<double>::max()), mUnreliableQueue(config.unreliableQueueCapacity, OverflowPolicy::DropOldest)
{
}


size_t ReliableLink::maxFragmentBody() const
{
	// Room for the tag is left in every fragment, so all fragments but the last have the same size
	return mConfig.maxDatagramSize > MaxHeaderSize ? mConfig.maxDatagramSize - MaxHeaderSize : 0;
}


size_t ReliableLink::maxUnreliableBody() const
{
	constexpr size_t overhead = BaseHeaderSize + TagExtensionSize;
	return mConfig.maxDatagramSize > overhead ? mConfig.maxDatagramSize - overhead : 0;
}


size_t ReliableLink::maxRangesPerDatagram() const
{
	constexpr size_t overhead = BaseHeaderSize + AckWindowFieldSize;
	return mConfig.maxDatagramSize > overhead + SeqRangeSize ? (mConfig.maxDatagramSize - overhead) / SeqRangeSize : 1;
}


ReliableLink::Stream &ReliableLink::streamFor(const ChannelId channel)
{
	auto &slot = mStreams[indexOf(channel)];

	if (!slot)
	{
		slot = channel == ChannelId::Control ? std::make_unique<Stream>(ControlQueueCapacity, OverflowPolicy::DropNewest, mConfig.maxMessageSize, maxFragmentBody())
											 : std::make_unique<Stream>(mConfig.sendQueueCapacity, mConfig.sendQueueOverflow, mConfig.maxMessageSize, maxFragmentBody());
	}

	return *slot;
}


// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

PushResult ReliableLink::queueReliable(const ChannelId channel, const uint32_t tag, std::vector<uint8_t> body)
{
	if (body.size() > mConfig.maxMessageSize || FragmentationService::fragmentCount(body.size(), maxFragmentBody()) == 0)
	{
		NETLINK_LOG_ERROR("Message of {} bytes exceeds the maximum message size", body.size());
		return PushResult::Rejected;
	}

	auto			&queue	 = streamFor(channel).queue;
	const bool		 wasFull = queue.full();
	const PushResult result	 = queue.push(OutboundMessage{.channel = channel, .tag = tag, .body = std::make_shared<const std::vector<uint8_t>>(std::move(body))});

	// Callers that respect backpressure ask again and again while the queue is full: reported once, when it fills up
	if (result == PushResult::EvictedOldest)
		NETLINK_LOG_DEBUG("Send queue full ({} messages), dropped the oldest unsent message", queue.capacity());
	else if (!wasFull && queue.full())
		NETLINK_LOG_DEBUG("Send queue full ({} messages)", queue.capacity());

	return result;
}


bool ReliableLink::sendUnreliable(const ChannelId channel, const uint32_t tag, const std::span<const uint8_t> body)
{
	if (body.size() > maxUnreliableBody())
		return false;

	PacketHeader header = makeHeader(PacketFlags::data(channel, false), mNextUnreliableSeq++);
	header.tag			= tag;

	mUnreliableQueue.push(makeDatagram(header, body));
	return true;
}


void ReliableLink::sendHeartbeat()
{
	mHeartbeatDue = true;
}


std::vector<OutgoingDatagram> ReliableLink::takeOutgoing(const TimePoint now)
{
	std::vector<OutgoingDatagram> pass;

	while (const auto *ack = peekAck())
	{
		pass.push_back(*ack);
		commitAck();
	}

	for (const SendClass sendClass : {SendClass::Unreliable, SendClass::Control, SendClass::Application})
	{
		while (const auto *datagram = peek(sendClass, now))
		{
			pass.push_back(*datagram);
			commit(sendClass, now);
		}
	}

	return pass;
}


const OutgoingDatagram *ReliableLink::peekAck()
{
	// Whatever was not sent yet goes first, and nothing new is built behind it
	if (mPendingAcks.empty())
	{
		for (const ChannelId channel : ChannelOrder)
		{
			if (auto *stream = existingStream(channel))
				flushAcks(*stream, channel);
		}

		if (std::exchange(mHeartbeatDue, false))
			mPendingAcks.push_back(makeDatagram(makeHeader(PacketFlags::heartbeat(), 0)));
	}

	return mPendingAcks.empty() ? nullptr : &mPendingAcks.front();
}


void ReliableLink::commitAck()
{
	if (!mPendingAcks.empty())
		mPendingAcks.pop_front();
}


const OutgoingDatagram *ReliableLink::peek(const SendClass sendClass, const TimePoint now)
{
	if (sendClass == SendClass::Unreliable)
		return mUnreliableQueue.empty() ? nullptr : &mUnreliableQueue.front();

	const ChannelId channel = channelOf(sendClass);
	Stream		   *stream	= existingStream(channel);
	if (!stream)
		return nullptr;

	const auto next = nextTransmission(*stream, channel, now);
	if (!next)
		return nullptr;

	const InFlight	fragment = next->lost ? InFlight{} : makeFragment(*stream, channel);
	const InFlight &entry	 = next->lost ? *next->lost : fragment;

	mPeeked					 = makeDatagram(entry.header);
	mPeeked.message			 = entry.message;
	mPeeked.offset			 = entry.offset;
	mPeeked.length			 = entry.length;
	return &mPeeked;
}


void ReliableLink::commit(const SendClass sendClass, const TimePoint now)
{
	if (sendClass == SendClass::Unreliable)
	{
		mUnreliableQueue.pop();
		return;
	}

	const ChannelId channel = channelOf(sendClass);
	Stream		   *stream	= existingStream(channel);
	if (!stream)
		return;

	const auto next = nextTransmission(*stream, channel, now);
	if (!next)
		return;

	InFlight *entry = next->lost;

	if (entry)
		stream->lost.pop_front();
	else
		entry = takeFragment(*stream, channel);

	if (next->probe)
		stream->probeAt = now + mConfig.windowProbeInterval;

	// The window was not what ended the sending, unless a later peek finds it closed
	if (channel == ChannelId::Application)
		mWindowLimited = false;

	transmit(*stream, *entry, now);
}


void ReliableLink::flushAcks(Stream &stream, const ChannelId channel)
{
	const size_t		 perDatagram = maxRangesPerDatagram();
	std::vector<uint8_t> body;

	if (!stream.dataAcks.empty())
	{
		// Everything that waits behind a gap is listed again with every DataAck, not only once when it arrived. A DataAck
		// that gets lost then costs nothing: the next one says the same and more, and the sender keeps seeing which seqs
		// are missing instead of sending again what already arrived.
		if (!stream.reorder.empty())
		{
			stream.reorder.forEach(
				[&](const uint64_t seq, const BufferedData &)
				{
					stream.dataAcks.push_back(seq);
					return true;
				});
		}

		const auto	   ranges = toRanges(stream.dataAcks);
		const uint16_t window = channel == ChannelId::Application && !mApplicationReceiving ? uint16_t{0} : static_cast<uint16_t>(WindowSize);

		for (size_t first = 0; first < ranges.size(); first += perDatagram)
		{
			body.assign(AckWindowFieldSize, 0);
			writeUint16(body.data(), window);

			for (size_t i = first; i < std::min(first + perDatagram, ranges.size()); ++i)
				appendRange(body, ranges[i]);

			mPendingAcks.push_back(makeDatagram(makeHeader(PacketFlags::ack(PacketKind::DataAck, channel), stream.nextExpected - 1), body));
			++mStats.dataAcksSent;
		}

		stream.dataAcks.clear();
	}

	if (std::exchange(stream.ackAckDue, false))
	{
		// At least one datagram, also without ranges: its seq confirms everything up to the send base
		size_t first = 0;
		do
		{
			body.clear();

			for (size_t i = first; i < std::min(first + perDatagram, stream.ackAcks.size()); ++i)
				appendRange(body, stream.ackAcks[i]);

			mPendingAcks.push_back(makeDatagram(makeHeader(PacketFlags::ack(PacketKind::AckAck, channel), stream.sendBase - 1), body));
			++mStats.ackAcksSent;

			first += perDatagram;
		} while (first < stream.ackAcks.size());

		stream.ackAcks.clear();
	}
}


bool ReliableLink::congestionWindowOpen(const Stream &stream, const ChannelId channel) const
{
	// Control signals are few and small, and a session has to be able to end while the window is full of application data
	return channel == ChannelId::Control || stream.onTheWire < static_cast<size_t>(mCongestionWindow);
}


std::optional<ReliableLink::Transmission> ReliableLink::nextTransmission(Stream &stream, const ChannelId channel, const TimePoint now)
{
	// Retransmissions first: the receiver cannot deliver what it buffered behind them
	Transmission next{.lost = nextLost(stream)};

	if (!next.lost && !hasSendable(stream))
		return std::nullopt;

	if (!congestionWindowOpen(stream, channel))
	{
		mWindowLimited = true;
		return std::nullopt;
	}

	if (stream.peerWindow > 0)
		return next;

	// The remote paused this channel. One packet per probe interval keeps asking: its acknowledgement carries the window.
	if (stream.onTheWire > 0)
		return std::nullopt;

	if (stream.probeAt && now < *stream.probeAt)
	{
		scheduleDeadline(*stream.probeAt);
		return std::nullopt;
	}

	next.probe = true;
	return next;
}


ReliableLink::InFlight *ReliableLink::nextLost(Stream &stream)
{
	while (!stream.lost.empty())
	{
		// Acknowledged while it was waiting: its entry is gone
		if (InFlight *entry = stream.inFlight.find(stream.lost.front()); entry && entry->lost)
			return entry;

		stream.lost.pop_front();
	}

	return nullptr;
}


bool ReliableLink::hasSendable(const Stream &stream)
{
	// All unacknowledged seqs stay within one window, which keeps the SequenceBuffer slots unique on both sides
	return (stream.cursor.has_value() || !stream.queue.empty()) && stream.nextSendSeq - stream.sendBase < WindowSize;
}


ReliableLink::InFlight ReliableLink::makeFragment(const Stream &stream, const ChannelId channel) const
{
	const OutboundMessage &message	= stream.cursor ? stream.cursor->message : stream.queue.front();
	const size_t		   index	= stream.cursor ? stream.cursor->next : 0;
	const Fragment		   fragment = FragmentationService::fragmentAt(*message.body, index, maxFragmentBody());

	InFlight			   entry;
	entry.header	 = makeHeader(PacketFlags::data(channel, true), stream.nextSendSeq);
	entry.header.tag = message.tag;

	if (fragment.isFragmented())
	{
		entry.header.flags.setFragment(true, fragment.isLast());
		entry.header.fragIndex = fragment.index;
		entry.header.fragCount = fragment.count;
	}

	entry.message = message.body;
	entry.offset  = static_cast<uint32_t>(fragment.body.data() - message.body->data());
	entry.length  = static_cast<uint32_t>(fragment.body.size());
	return entry;
}


ReliableLink::InFlight *ReliableLink::takeFragment(Stream &stream, const ChannelId channel) const
{
	InFlight entry = makeFragment(stream, channel);

	if (!stream.cursor)
	{
		auto		 message = stream.queue.pop();
		const size_t count	 = FragmentationService::fragmentCount(message->body->size(), maxFragmentBody());
		stream.cursor		 = FragmentCursor{.message = std::move(*message), .count = count, .next = 0};
	}

	if (++stream.cursor->next >= stream.cursor->count)
		stream.cursor.reset();

	return &stream.inFlight.insert(stream.nextSendSeq++, std::move(entry));
}


void ReliableLink::transmit(Stream &stream, InFlight &entry, const TimePoint now)
{
	if (entry.transmissions == 0)
		++mStats.dataSent;
	else
		++mStats.retransmissions;

	// The timer runs from the moment the fragment really goes out. It backs off while nothing at all is acknowledged,
	// not per packet: on a path that merely loses packets, one that was unlucky twice is not sent any later for it.
	entry.lost		   = false;
	entry.sentAt	   = now;
	entry.deadline	   = now + mRtt.timeoutFor(mTimeoutsInARow);
	entry.transmission = ++mTransmissions;
	++entry.transmissions;
	scheduleDeadline(entry.deadline);

	// The failure clock starts with the first packet that waits for its acknowledgement
	if (!mStalledSince)
	{
		mStalledSince = now;
		scheduleDeadline(now + mConfig.failureTimeout);
	}

	++stream.onTheWire;
}


// ---------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------

void ReliableLink::onPacket(const DecodedPacket &packet, const TimePoint now)
{
	const PacketHeader &header = packet.header;

	// Addressed to an earlier stream of this link
	if (header.dstStreamID != 0 && header.dstStreamID != mLocalStreamID)
	{
		++mStats.staleDropped;
		return;
	}

	if (!mRemoteStreamID)
	{
		mRemoteStreamID = header.srcStreamID;
	}
	else if (*mRemoteStreamID != header.srcStreamID)
	{
		NETLINK_LOG_INFO("Peer restarted (stream ID {} -> {}), resetting the link", *mRemoteStreamID, header.srcStreamID);

		// Whatever was in flight belonged to a session the peer no longer knows
		resetStreams();
		mRemoteStreamID = header.srcStreamID;
		mEvents.push_back(LinkEvent::PeerRestarted);
	}

	const ChannelId channel = header.flags.channel();

	switch (header.flags.kind())
	{
	case PacketKind::Data:
		if (header.flags.isReliable())
			handleReliableData(channel, header, packet.body, now);
		else
			handleUnreliableData(header, packet.body);
		break;

	// Acknowledgements for a channel nothing was ever sent on cannot be meant for this stream
	case PacketKind::DataAck:
		if (auto *stream = existingStream(channel))
			handleDataAck(*stream, channel, header, packet.body, now);
		break;

	case PacketKind::AckAck:
		if (auto *stream = existingStream(channel))
			handleAckAck(*stream, header, packet.body);
		break;

	case PacketKind::Heartbeat: break; // Liveness only, tracked by the owner
	}
}


void ReliableLink::handleReliableData(const ChannelId channel, const PacketHeader &header, const std::span<const uint8_t> body, const TimePoint now)
{
	const uint64_t seq = header.seq;

	if (seq == 0)
		return;

	Stream &stream = streamFor(channel);

	// No room to buffer it: without an ack the sender retransmits once the window moved on
	if (seq >= stream.nextExpected + WindowSize)
	{
		++mStats.outOfWindowDropped;
		return;
	}

	// Every Data packet is acknowledged, duplicates too: the earlier DataAck may have been lost
	if (stream.dataAcks.size() < MaxPendingDataAcks)
		stream.dataAcks.push_back(seq);

	if (seq < stream.nextExpected || stream.reorder.contains(seq))
	{
		++mStats.duplicatesReceived;
		return;
	}

	AckRecord record;
	record.deadline = now + mRtt.timeoutFor(0);
	stream.ackRecords.insert(seq, record);
	scheduleDeadline(record.deadline);

	// Out of order: kept until the gap before it is closed
	if (seq != stream.nextExpected)
	{
		stream.reorder.insert(seq, BufferedData{.header = header, .body = std::vector<uint8_t>(body.begin(), body.end())});
		return;
	}

	// In order, the normal case: straight from the datagram into its message
	acceptInOrder(stream, channel, header, body);
	++stream.nextExpected;

	while (auto ready = stream.reorder.take(stream.nextExpected))
	{
		acceptInOrder(stream, channel, ready->header, ready->body);
		++stream.nextExpected;
	}
}


void ReliableLink::acceptInOrder(Stream &stream, const ChannelId channel, const PacketHeader &header, const std::span<const uint8_t> body)
{
	if (auto message = stream.assembler.accept(header, body))
	{
		mDelivered.push_back(DeliveredMessage{.channel = channel, .tag = message->tag, .body = std::move(message->body)});
		++mStats.delivered;
	}
}


void ReliableLink::handleUnreliableData(const PacketHeader &header, const std::span<const uint8_t> body)
{
	// Sequenced: anything older than what was already delivered is stale
	if (header.seq <= mLastUnreliableSeq)
	{
		++mStats.staleDropped;
		return;
	}

	mLastUnreliableSeq = header.seq;
	mDelivered.push_back(DeliveredMessage{.channel = header.flags.channel(), .tag = header.tag, .body = std::vector<uint8_t>(body.begin(), body.end())});
	++mStats.delivered;
}


void ReliableLink::handleDataAck(Stream &stream, const ChannelId channel, const PacketHeader &header, const std::span<const uint8_t> body, const TimePoint now)
{
	if (body.size() < AckWindowFieldSize)
		return;

	const auto ranges = decodeRanges(body.subspan(AckWindowFieldSize));
	if (!ranges)
		return;

	stream.peerWindow	= readUint16(body.data());

	bool	  hasSample = false;
	TimePoint sampleSentAt{};

	mAcknowledged.clear();

	const auto release = [&](const uint64_t seq)
	{
		const auto entry = stream.inFlight.take(seq);
		if (!entry)
			return;

		if (!entry->lost)
			--stream.onTheWire;

		mLargestAcked = std::max(mLargestAcked, entry->transmission);
		mAcknowledged.push_back(entry->transmission);

		// Karn: only unambiguous samples. One per DataAck is enough: the packet that was sent last.
		if (entry->transmissions == 1 && (!hasSample || entry->sentAt > sampleSentAt))
		{
			hasSample	 = true;
			sampleSentAt = entry->sentAt;
		}
	};

	const uint64_t lastSent = stream.nextSendSeq - 1;

	// The header acknowledges everything the remote received without a gap. This also covers DataAcks that got lost.
	const uint64_t inOrder	= std::min(header.seq, lastSent);
	for (uint64_t seq = stream.sendBase; seq <= inOrder; ++seq)
		release(seq);

	stream.highestAcked = std::max(stream.highestAcked, inOrder);

	for (const SeqRange &range : *ranges)
	{
		// Never sent by us
		if (range.first > lastSent)
			continue;

		const uint64_t last = std::min(range.last(), lastSent);
		for (uint64_t seq = std::max(range.first, stream.sendBase); seq <= last; ++seq)
			release(seq);

		stream.highestAcked = std::max(stream.highestAcked, last);

		// Always confirmed, also for seqs already completed: the receiver missed the earlier AckAck
		if (stream.ackAcks.size() < MaxPendingAckAcks)
			stream.ackAcks.push_back({.first = range.first, .count = static_cast<uint16_t>(last - range.first + 1)});
	}

	stream.ackAckDue = true;
	advanceSendBase(stream);

	if (mAcknowledged.empty())
		return;

	if (hasSample)
		mRtt.addSample(std::chrono::duration_cast<RttEstimator::Duration>(now - sampleSentAt));

	// Progress: the failure clock starts over, or stops when nothing is left to wait for
	if (inFlightCount() > 0)
		mStalledSince = now;
	else
		mStalledSince.reset();

	mTimeoutsInARow = 0;

	// While packets are missing behind acknowledged ones the window does not grow: they are probably lost
	if (detectLosses(stream))
		return;

	// Control signals never wait for the window, so their acknowledgements say nothing about how much it can take
	if (channel != ChannelId::Application)
		return;

	// Packets from before the window was last reduced say nothing about the reduced window: only what was sent since counts
	growCongestionWindow(static_cast<size_t>(std::ranges::count_if(mAcknowledged, [this](const uint64_t transmission) { return transmission > mRecoveryStart; })));
}


void ReliableLink::handleAckAck(Stream &stream, const PacketHeader &header, const std::span<const uint8_t> body)
{
	const auto ranges = decodeRanges(body);
	if (!ranges)
		return;

	// A record only exists for the newest seq of its slot: older ones than a window ago are gone anyway
	const auto forget = [&stream](const uint64_t first, const uint64_t last)
	{
		for (uint64_t seq = last - first >= WindowSize ? last - WindowSize + 1 : first; seq <= last; ++seq)
			stream.ackRecords.erase(seq);
	};

	// The header confirms every DataAck up to the sender's send base. This also covers AckAcks that got lost.
	if (const uint64_t through = std::min(header.seq, stream.nextExpected - 1); through > stream.ackAckedThrough)
	{
		forget(stream.ackAckedThrough + 1, through);
		stream.ackAckedThrough = through;
	}

	for (const SeqRange &range : *ranges)
	{
		// Never received by us
		if (range.first >= stream.nextExpected + WindowSize)
			continue;

		forget(range.first, std::min(range.last(), stream.nextExpected + WindowSize - 1));
	}
}


// ---------------------------------------------------------------------------
// Loss and congestion
// ---------------------------------------------------------------------------

void ReliableLink::advanceSendBase(Stream &stream)
{
	while (stream.sendBase < stream.nextSendSeq && !stream.inFlight.contains(stream.sendBase))
		++stream.sendBase;
}


bool ReliableLink::detectLosses(Stream &stream)
{
	bool missing = false;

	// Only seqs below an acknowledged one can be judged: something sent after them arrived
	for (uint64_t seq = stream.sendBase; seq < stream.highestAcked; ++seq)
	{
		InFlight *entry = stream.inFlight.find(seq);

		// By transmission, not by seq: a retransmission is only lost again once packets sent after it are acknowledged
		if (!entry || entry->lost || entry->transmission + mConfig.reorderThreshold > mLargestAcked)
			continue;

		missing = true;
		markLost(stream, seq, *entry, false);
	}

	return missing;
}


void ReliableLink::markLost(Stream &stream, const uint64_t seq, InFlight &entry, const bool timedOut)
{
	entry.lost = true;
	--stream.onTheWire;
	stream.lost.push_back(seq);

	if (!timedOut)
		++mStats.fastRetransmissions;

	// One reduction per round of losses: packets that were on the wire before the window shrank do not shrink it again
	if (entry.transmission <= mRecoveryStart)
		return;

	const auto floor	= static_cast<double>(mConfig.minCongestionWindow);
	mSlowStartThreshold = std::max(mCongestionWindow / 2, floor);

	// A timeout means the acknowledgements stopped coming altogether: start over carefully
	mCongestionWindow	= timedOut ? floor : mSlowStartThreshold;
	mRecoveryStart		= mTransmissions;
}


void ReliableLink::growCongestionWindow(const size_t acknowledged)
{
	// A window that was not used up says nothing about what the path can take
	if (!mWindowLimited || acknowledged == 0)
		return;

	if (mCongestionWindow < mSlowStartThreshold)
		mCongestionWindow += static_cast<double>(acknowledged);
	else
		mCongestionWindow += static_cast<double>(acknowledged) / mCongestionWindow;

	mCongestionWindow = std::min(mCongestionWindow, static_cast<double>(mConfig.maxCongestionWindow));
}


void ReliableLink::fail()
{
	NETLINK_LOG_WARNING("Link failed: nothing was acknowledged for {} ms", mConfig.failureTimeout.count());

	// The stream cannot continue with a gap: start a new one under a new stream ID, which tells the remote to reset as well
	mLocalStreamID = makeStreamID(mLocalStreamID);
	mRemoteStreamID.reset();
	resetStreams();
	mEvents.push_back(LinkEvent::Failed);
}


// ---------------------------------------------------------------------------
// Timers
// ---------------------------------------------------------------------------

void ReliableLink::onTimer(const TimePoint now)
{
	if (!mNextDeadline || now < *mNextDeadline)
		return;

	mNextDeadline.reset();

	if (mStalledSince)
	{
		if (now - *mStalledSince >= mConfig.failureTimeout)
		{
			fail();
			return;
		}

		scheduleDeadline(*mStalledSince + mConfig.failureTimeout);
	}

	std::vector<uint64_t> overdue;
	bool				  timedOut = false;

	for (const ChannelId channel : ChannelOrder)
	{
		Stream *stream = existingStream(channel);
		if (!stream)
			continue;

		overdue.clear();

		stream->inFlight.forEach(
			[&](const uint64_t seq, const InFlight &entry)
			{
				// Waiting for its retransmission: no timer is running
				if (entry.lost)
					return true;

				if (now >= entry.deadline)
					overdue.push_back(seq);
				else
					scheduleDeadline(entry.deadline);

				return true;
			});

		// The oldest first: the receiver waits for it to deliver everything behind
		std::ranges::sort(overdue);

		for (const uint64_t seq : overdue)
			markLost(*stream, seq, *stream->inFlight.find(seq), true);

		timedOut |= !overdue.empty();

		stream->ackRecords.forEach(
			[&](const uint64_t seq, AckRecord &record)
			{
				if (now >= record.deadline)
				{
					// Best effort: the sender's Data retransmission triggers a fresh DataAck anyway
					if (record.retransmits >= mConfig.maxAckRetransmits)
						return false;

					++record.retransmits;
					record.deadline = now + mRtt.timeoutFor(record.retransmits);

					if (stream->dataAcks.size() < MaxPendingDataAcks)
						stream->dataAcks.push_back(seq);
				}

				scheduleDeadline(record.deadline);
				return true;
			});

		// Paused by the remote with data waiting: the next probe is due
		if (stream->peerWindow == 0 && stream->probeAt && now < *stream->probeAt && (hasSendable(*stream) || !stream->lost.empty()))
			scheduleDeadline(*stream->probeAt);
	}

	// Another round without any acknowledgement: wait longer before the next one (capped by the maximum timeout)
	if (timedOut && mTimeoutsInARow < MaxBackoffSteps)
		++mTimeoutsInARow;
}


void ReliableLink::scheduleDeadline(const TimePoint deadline)
{
	if (!mNextDeadline || deadline < *mNextDeadline)
		mNextDeadline = deadline;
}


// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

PacketHeader ReliableLink::makeHeader(const PacketFlags flags, const uint64_t seq) const
{
	PacketHeader header;
	header.flags	   = flags;
	header.srcStreamID = mLocalStreamID;
	header.dstStreamID = mRemoteStreamID.value_or(0);
	header.seq		   = seq;
	return header;
}


OutgoingDatagram ReliableLink::makeDatagram(const PacketHeader &header) const
{
	// Re-stamped on every transmission: stream IDs may have become known since the first one
	PacketHeader stamped = header;
	stamped.srcStreamID	 = mLocalStreamID;
	stamped.dstStreamID	 = mRemoteStreamID.value_or(0);

	OutgoingDatagram datagram;
	datagram.headSize = static_cast<uint8_t>(encodeHeader(stamped, datagram.head.data()));
	return datagram;
}


OutgoingDatagram ReliableLink::makeDatagram(const PacketHeader &header, const std::span<const uint8_t> body) const
{
	OutgoingDatagram datagram = makeDatagram(header);
	datagram.owned.assign(body.begin(), body.end());
	return datagram;
}


bool ReliableLink::hasRoomFor(const ChannelId channel) const
{
	const Stream *stream = existingStream(channel);
	return !stream || !stream->queue.full() || stream->queue.policy() == OverflowPolicy::DropOldest;
}


bool ReliableLink::hasPendingReliable() const
{
	return std::ranges::any_of(mStreams, [](const auto &stream) { return stream && (!stream->inFlight.empty() || stream->cursor.has_value() || !stream->queue.empty()); });
}


bool ReliableLink::hasOutgoing() const
{
	if (mHeartbeatDue || !mPendingAcks.empty() || !mUnreliableQueue.empty())
		return true;

	return std::ranges::any_of(ChannelOrder,
							   [&](const ChannelId channel)
							   {
								   const Stream *stream = existingStream(channel);
								   if (!stream)
									   return false;

								   if (!stream->dataAcks.empty() || stream->ackAckDue)
									   return true;

								   return congestionWindowOpen(*stream, channel) && stream->peerWindow > 0 && (hasSendable(*stream) || !stream->lost.empty());
							   });
}


size_t ReliableLink::inFlightCount() const
{
	size_t count = 0;
	for (const auto &stream : mStreams)
		count += stream ? stream->inFlight.size() : 0;
	return count;
}


size_t ReliableLink::queuedMessageCount() const
{
	size_t count = 0;
	for (const auto &stream : mStreams)
		count += stream ? stream->queue.size() + (stream->cursor ? 1 : 0) : 0;
	return count;
}


void ReliableLink::dropQueuedApplicationMessages()
{
	Stream *stream = existingStream(ChannelId::Application);
	if (!stream)
		return;

	stream->queue.clear();

	// A message that is partly on the wire cannot be completed anymore either; the receiver abandons the partial message
	stream->cursor.reset();
}


void ReliableLink::resetStreams()
{
	// Nothing of the old streams may still go out, and the new ones learn the connection quality anew
	for (auto &stream : mStreams)
		stream.reset();

	mRtt.reset();
	mCongestionWindow	= static_cast<double>(std::clamp(mConfig.initialCongestionWindow, mConfig.minCongestionWindow, mConfig.maxCongestionWindow));
	mSlowStartThreshold = std::numeric_limits<double>::max();
	mTransmissions		= 0;
	mLargestAcked		= 0;
	mRecoveryStart		= 0;
	mWindowLimited		= false;
	mTimeoutsInARow		= 0;
	mStalledSince.reset();

	mPendingAcks.clear();
	mUnreliableQueue.clear();
	mNextUnreliableSeq = 1;
	mLastUnreliableSeq = 0;
	mHeartbeatDue	   = false;
	mNextDeadline.reset();
}

} // namespace netlink::channel
