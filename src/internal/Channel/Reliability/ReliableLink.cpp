/*
  ==============================================================================
	Module:         ReliableLink
	Description:    Message streams to one remote peer on top of datagrams:
					three acknowledged, ordered lanes and one that is neither
  ==============================================================================
*/

#include "ReliableLink.h"

#include <algorithm>
#include <random>
#include <utility>

#include "NetLinkLog.h"


namespace netlink::channel
{

namespace
{

// Doublings of the retransmission timeout: far more than it takes to reach any maximum timeout
constexpr int  MaxBackoffSteps = 16;

constexpr Lane StreamLanes[]   = {Lane::Control, Lane::Reliable, Lane::Bulk};

constexpr bool sharesCongestionWindow(const Lane lane)
{
	return lane == Lane::Reliable || lane == Lane::Bulk;
}

double initialWindow(const LinkTimings &timings)
{
	return static_cast<double>(timings.fixedCwnd > 0 ? timings.fixedCwnd : std::clamp(timings.initialCwnd, MinCongestionWindow, MaxCongestionWindow));
}

} // namespace


uint32_t makeStreamID()
{
	thread_local std::mt19937 generator = []
	{
		std::random_device device;
		std::seed_seq	   seed{device(), device(), device(), device(), device(), device(), device(), device()};
		return std::mt19937(seed);
	}();

	return std::uniform_int_distribution<uint32_t>(1, UINT32_MAX)(generator);
}


ReliableLink::ReliableLink(const LinkTimings &timings, const uint32_t localStreamID, AssemblyBudget *budget)
	: mTimings(timings), mLocalStreamID(localStreamID != 0 ? localStreamID : makeStreamID()), mRtt(timings.initialRto, timings.minRto, timings.maxRto), mBudget(budget),
	  mCongestionWindow(initialWindow(timings))
{
}


ReliableLink::Stream &ReliableLink::streamFor(const Lane lane)
{
	auto &slot = mStreams[std::to_underlying(lane)];

	if (!slot)
		slot = std::make_unique<Stream>(windowOf(lane), mBudget);

	return *slot;
}


// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

void ReliableLink::supervise(const TimePoint now)
{
	mLastReceived = now;
	mPingedAt	  = now;
}


const OutgoingDatagram *ReliableLink::peekAck()
{
	if (mFailed)
		return nullptr;

	if (mPendingAcks.empty())
	{
		for (const Lane lane : StreamLanes)
		{
			if ((mAckDue & bitOf(lane)) != 0)
				mPendingAcks.push_back(makeAck(lane));

			if ((mPingDue & bitOf(lane)) != 0)
			{
				mPendingAcks.push_back(makeDatagram(makeHeader(PacketFlags::ping(lane), 0)));
				++mStats.pingsSent;
			}
		}

		mAckDue	 = 0;
		mPingDue = 0;
	}

	return mPendingAcks.empty() ? nullptr : &mPendingAcks.front();
}


void ReliableLink::commitAck()
{
	if (!mPendingAcks.empty())
		mPendingAcks.pop_front();
}


OutgoingDatagram ReliableLink::makeAck(const Lane lane)
{
	Stream *stream = existingStream(lane);

	if (stream)
		retryRefused(*stream, lane);

	AckBody ack{.serial = ++mAckSerial, .mediaReceived = mMediaReceived, .ranges = {}};

	// Only what waits behind a gap is listed: everything before it is named by the seq in the header
	if (stream && !stream->reorder.empty())
	{
		std::vector<uint64_t> waiting;
		waiting.reserve(stream->reorder.size());

		stream->reorder.forEach(
			[&waiting](const uint64_t seq, const BufferedData &)
			{
				waiting.push_back(seq);
				return true;
			});

		ack.ranges = toRanges(waiting);
	}

	const bool		 paused	  = (mPaused && lane != Lane::Control) || (stream && stream->budgetPaused);
	OutgoingDatagram datagram = makeDatagram(makeHeader(PacketFlags::ack(lane, paused), stream ? stream->nextExpected - 1 : 0));
	datagram.owned			  = encodeAck(ack, MaxAckRanges);

	++mStats.acksSent;
	return datagram;
}


const OutgoingDatagram *ReliableLink::peek(const Lane lane, const TimePoint, MessageSource &source)
{
	if (mFailed)
		return nullptr;

	if (lane == Lane::Media)
		return peekMedia(source);

	Stream	 *stream = existingStream(lane);
	InFlight *lost	 = nullptr;

	if (stream)
	{
		// Retransmissions first: the receiver cannot deliver what it buffered behind them
		lost = nextLost(*stream);

		// All unacknowledged seqs stay within one window, which keeps the SequenceBuffer slots unique on both sides
		if (!lost && stream->nextSendSeq - stream->sendBase >= stream->inFlight.capacity())
			return nullptr;

		if (stream->peerPaused)
			return nullptr;
	}

	if (!congestionWindowOpen(lane))
		return nullptr;

	if (lost)
		return offer(headerOf(lane, stream->lost.front(), *lost), *lost);

	// The next message is only taken once its first fragment can go out
	if (!stream || !stream->cursor)
	{
		auto message = source.next(lane);
		if (!message)
			return nullptr;

		if (!stream)
			stream = &streamFor(lane);

		stream->cursor = cursorFor(std::move(*message));
	}

	const InFlight fragment = fragmentAt(*stream->cursor);
	return offer(headerOf(lane, stream->nextSendSeq, fragment), fragment);
}


const OutgoingDatagram *ReliableLink::peekMedia(MessageSource &source)
{
	if (!mMediaCursor)
	{
		auto message = source.next(Lane::Media);
		if (!message)
			return nullptr;

		mMediaCursor = cursorFor(std::move(*message));
	}

	const InFlight fragment = fragmentAt(*mMediaCursor);
	return offer(headerOf(Lane::Media, mNextMediaSeq, fragment), fragment);
}


void ReliableLink::commit(const Lane lane, const TimePoint now)
{
	if (lane == Lane::Media)
	{
		if (!mMediaCursor)
			return;

		++mNextMediaSeq;
		++mStats.mediaSent;
		mStats.bytesSent += mPeeked.length;

		if (++mMediaCursor->next >= mMediaCursor->count)
			mMediaCursor.reset();

		return;
	}

	Stream *stream = existingStream(lane);
	if (!stream)
		return;

	if (InFlight *lost = nextLost(*stream))
	{
		const uint64_t seq = stream->lost.front();
		stream->lost.pop_front();
		transmit(*stream, lane, seq, *lost, now);
		return;
	}

	if (!stream->cursor)
		return;

	const uint64_t seq	 = stream->nextSendSeq++;
	InFlight	  &entry = stream->inFlight.insert(seq, fragmentAt(*stream->cursor));

	if (++stream->cursor->next >= stream->cursor->count)
		stream->cursor.reset();

	transmit(*stream, lane, seq, entry, now);
}


bool ReliableLink::congestionWindowOpen(const Lane lane) const
{
	if (!sharesCongestionWindow(lane))
		return true;

	const Stream *reliable = existingStream(Lane::Reliable);
	const Stream *bulk	   = existingStream(Lane::Bulk);

	return (reliable ? reliable->onTheWire : 0) + (bulk ? bulk->onTheWire : 0) < static_cast<size_t>(mCongestionWindow);
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


ReliableLink::FragmentCursor ReliableLink::cursorFor(OutboundMessage message)
{
	const size_t count = fragmentsOf(message.body->size());
	return {.message = std::move(message), .count = count, .next = 0};
}


ReliableLink::InFlight ReliableLink::fragmentAt(const FragmentCursor &cursor)
{
	const size_t size	= cursor.message.body->size();
	const size_t offset = cursor.next * MaxFragmentBody;

	InFlight	 entry;
	entry.message	  = cursor.message.body;
	entry.offset	  = static_cast<uint32_t>(offset);
	entry.length	  = static_cast<uint32_t>(std::min(MaxFragmentBody, size - offset));
	entry.tag		  = cursor.message.tag;
	entry.totalLength = static_cast<uint32_t>(size);
	entry.fragIndex	  = static_cast<uint16_t>(cursor.next);
	entry.fragCount	  = static_cast<uint16_t>(cursor.count);
	return entry;
}


PacketHeader ReliableLink::headerOf(const Lane lane, const uint64_t seq, const InFlight &entry) const
{
	PacketHeader header = makeHeader(PacketFlags::data(lane), seq);
	header.tag			= entry.tag;

	if (entry.fragCount > 1)
	{
		header.flags.setFragment(true, entry.fragIndex + 1 == entry.fragCount);
		header.fragIndex   = entry.fragIndex;
		header.fragCount   = entry.fragCount;
		header.totalLength = entry.totalLength;
	}

	return header;
}


const OutgoingDatagram *ReliableLink::offer(const PacketHeader &header, const InFlight &entry)
{
	mPeeked			= makeDatagram(header);
	mPeeked.message = entry.message;
	mPeeked.offset	= entry.offset;
	mPeeked.length	= entry.length;
	return &mPeeked;
}


void ReliableLink::transmit(Stream &stream, const Lane lane, const uint64_t seq, InFlight &entry, const TimePoint now)
{
	if (entry.transmissions == 0)
		++mStats.dataSent;
	else
		++mStats.retransmissions;

	entry.lost		   = false;
	entry.sentAt	   = now;
	entry.transmission = ++mTransmissions;
	++entry.transmissions;
	++stream.onTheWire;

	if (entry.transmissions == 1 && lane != Lane::Control)
		mStats.bytesSent += entry.length;

	mSent.push_back({.transmission = entry.transmission, .seq = seq, .lane = lane});
	updateProgressClock(now, false);
}


// ---------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------

void ReliableLink::onPacket(const DecodedPacket &packet, const TimePoint now)
{
	const PacketHeader &header = packet.header;

	if (mFailed)
		return;

	// Of another stream than the one this link talks to
	if ((header.dstStreamID != 0 && header.dstStreamID != mLocalStreamID) || (mRemoteStreamID && *mRemoteStreamID != header.srcStreamID))
	{
		++mStats.staleDropped;
		return;
	}

	mRemoteStreamID = header.srcStreamID;

	if (mLastReceived)
		mLastReceived = now;

	const Lane lane = header.flags.lane();

	switch (header.flags.kind())
	{
	case PacketKind::Data:
		if (lane == Lane::Media)
			handleMedia(header, packet.body);
		else
			handleData(lane, header, packet.body);
		break;

	// An Ack for a lane nothing was ever sent on cannot be meant for this stream
	case PacketKind::Ack:
		if (auto *stream = existingStream(lane))
			handleAck(*stream, lane, header, packet.body, now);
		break;

	case PacketKind::Ping: mAckDue |= bitOf(lane); break;

	case PacketKind::Beacon: break;
	}
}


void ReliableLink::handleData(const Lane lane, const PacketHeader &header, const std::span<const uint8_t> body)
{
	Stream		  &stream = streamFor(lane);
	const uint64_t seq	  = header.seq;

	retryRefused(stream, lane);

	// No room to buffer it: without an Ack the sender retransmits once the window moved on
	if (seq >= stream.nextExpected + stream.reorder.capacity())
	{
		++mStats.outOfWindowDropped;
		return;
	}

	// Duplicates are answered too: the earlier Ack may have been lost
	mAckDue |= bitOf(lane);

	if (seq < stream.nextExpected || stream.reorder.contains(seq))
	{
		++mStats.duplicatesReceived;
		return;
	}

	if (seq != stream.nextExpected)
	{
		stream.reorder.insert(seq, BufferedData{.header = header, .body = std::vector<uint8_t>(body.begin(), body.end())});
		return;
	}

	if (!acceptInOrder(stream, lane, header, body))
		return;

	++stream.nextExpected;
	drainReorderBuffer(stream, lane);
}


bool ReliableLink::acceptInOrder(Stream &stream, const Lane lane, const PacketHeader &header, const std::span<const uint8_t> body)
{
	// No room for a message of that size right now: it is not taken, and the sender is told to wait
	if (!stream.assembler.hasRoomFor(header))
	{
		stream.budgetPaused	 = true;
		stream.refusedLength = header.totalLength;
		return false;
	}

	if (auto message = stream.assembler.accept(header, body))
	{
		mStats.bytesReceived += lane != Lane::Control ? message->body.size() : 0;
		mDelivered.push_back(DeliveredMessage{.lane = lane, .tag = message->tag, .body = std::move(message->body)});
		++mStats.delivered;
	}

	return true;
}


void ReliableLink::drainReorderBuffer(Stream &stream, const Lane lane)
{
	while (const BufferedData *buffered = stream.reorder.find(stream.nextExpected))
	{
		if (!acceptInOrder(stream, lane, buffered->header, buffered->body))
			return;

		stream.reorder.erase(stream.nextExpected);
		++stream.nextExpected;
	}
}


void ReliableLink::retryRefused(Stream &stream, const Lane lane)
{
	if (!stream.budgetPaused || (mBudget && !mBudget->fits(stream.refusedLength)))
		return;

	stream.budgetPaused = false;
	drainReorderBuffer(stream, lane);
	mAckDue |= bitOf(lane);
}


void ReliableLink::handleMedia(const PacketHeader &header, const std::span<const uint8_t> body)
{
	++mMediaReceived;
	++mStats.mediaReceived;

	if (!mMediaAssembler)
		mMediaAssembler = std::make_unique<MediaAssembler>();

	if (auto message = mMediaAssembler->accept(header, body))
	{
		mStats.bytesReceived += message->body.size();
		mDelivered.push_back(DeliveredMessage{.lane = Lane::Media, .tag = message->tag, .body = std::move(message->body)});
		++mStats.delivered;
	}
}


void ReliableLink::handleAck(Stream &stream, const Lane lane, const PacketHeader &header, const std::span<const uint8_t> body, const TimePoint now)
{
	const auto ack = decodeAck(body);
	if (!ack)
		return;

	// An Ack that was overtaken by a later one must not undo what that one said
	if (!stream.ackSerial || static_cast<int32_t>(ack->serial - *stream.ackSerial) > 0)
	{
		stream.ackSerial		   = ack->serial;
		mStats.mediaReceivedByPeer = ack->mediaReceived;
		setPeerPaused(stream, header.flags.isPaused(), now);
	}

	size_t	   acked	 = 0;
	size_t	   counted	 = 0; // ... of these: sent since the window was last reduced
	bool	  hasSample = false;
	TimePoint sampleSentAt{};

	const auto release = [&](const uint64_t seq)
	{
		const auto entry = stream.inFlight.take(seq);
		if (!entry)
			return;

		if (!entry->lost)
			--stream.onTheWire;

		mLargestAcked = std::max(mLargestAcked, entry->transmission);
		++acked;
		counted += entry->transmission > mRecoveryStart ? 1 : 0;

		// Karn: only unambiguous samples. One per Ack is enough: the packet that was sent last.
		if (entry->transmissions == 1 && (!hasSample || entry->sentAt > sampleSentAt))
		{
			hasSample	 = true;
			sampleSentAt = entry->sentAt;
		}
	};

	const uint64_t lastSent = stream.nextSendSeq - 1;

	for (uint64_t seq = stream.sendBase; seq <= std::min(header.seq, lastSent); ++seq)
		release(seq);

	for (const SeqRange &range : ack->ranges)
	{
		if (range.first > lastSent)
			continue;

		for (uint64_t seq = std::max(range.first, stream.sendBase); seq <= std::min(range.last(), lastSent); ++seq)
			release(seq);
	}

	while (stream.sendBase < stream.nextSendSeq && !stream.inFlight.contains(stream.sendBase))
		++stream.sendBase;

	if (acked > 0)
	{
		if (hasSample)
			mRtt.addSample(std::chrono::duration_cast<RttEstimator::Duration>(now - sampleSentAt));

		mTimeoutsInARow = 0;
	}

	const bool losses = detectLosses();
	updateProgressClock(now, acked > 0);

	// While packets are missing behind acknowledged ones the window does not grow
	if (sharesCongestionWindow(lane) && !losses && counted > 0 && mTimings.fixedCwnd == 0)
		mCongestionWindow = std::min(mCongestionWindow + static_cast<double>(counted) / mCongestionWindow, static_cast<double>(MaxCongestionWindow));
}


void ReliableLink::setPaused(const bool paused)
{
	if (mPaused == paused)
		return;

	mPaused = paused;

	// The remote learns about it right away, not only with the next packet it happens to send
	for (const Lane lane : {Lane::Reliable, Lane::Bulk})
	{
		if (existingStream(lane))
			mAckDue |= bitOf(lane);
	}
}


// ---------------------------------------------------------------------------
// Loss and congestion
// ---------------------------------------------------------------------------

ReliableLink::InFlight *ReliableLink::entryOf(const SentRecord &record)
{
	Stream *stream = existingStream(record.lane);
	if (!stream)
		return nullptr;

	// Acknowledged, declared lost or sent again in the meantime: the record is of no use anymore
	InFlight *entry = stream->inFlight.find(record.seq);
	return entry && !entry->lost && entry->transmission == record.transmission ? entry : nullptr;
}


bool ReliableLink::detectLosses()
{
	bool lost = false;

	while (!mSent.empty())
	{
		const SentRecord record = mSent.front();
		InFlight		*entry	= entryOf(record);

		if (entry && record.transmission + FastRetransmitThreshold > mLargestAcked)
			break;

		mSent.pop_front();

		if (entry)
		{
			markLost(record, *existingStream(record.lane), *entry, false);
			lost = true;
		}
	}

	return lost;
}


void ReliableLink::markLost(const SentRecord &record, Stream &stream, InFlight &entry, const bool timedOut)
{
	entry.lost = true;
	--stream.onTheWire;
	stream.lost.push_back(record.seq);

	if (!timedOut)
		++mStats.fastRetransmissions;

	// One reduction per round of losses: packets that were on the wire before the window shrank do not shrink it again
	if (!sharesCongestionWindow(record.lane) || mTimings.fixedCwnd > 0 || entry.transmission <= mRecoveryStart)
		return;

	mCongestionWindow = std::max(mCongestionWindow / 2, static_cast<double>(MinCongestionWindow));
	mRecoveryStart	  = mTransmissions;
}


void ReliableLink::setPeerPaused(Stream &stream, const bool paused, const TimePoint now)
{
	if (stream.peerPaused == paused)
		return;

	stream.peerPaused = paused;
	stream.probeAt.reset();

	if (paused)
	{
		stream.probeInterval = std::max(std::chrono::duration_cast<std::chrono::milliseconds>(mRtt.rto()), MinPauseProbeInterval);
		stream.probeAt		 = now + stream.probeInterval;
	}

	updateProgressClock(now, false);
}


void ReliableLink::updateProgressClock(const TimePoint now, const bool progress)
{
	// Data on a lane the remote paused is not expected to be acknowledged
	const bool waiting = std::ranges::any_of(mStreams, [](const auto &stream) { return stream && !stream->peerPaused && !stream->inFlight.empty(); });

	if (!waiting)
		mStalledSince.reset();
	else if (progress || !mStalledSince)
		mStalledSince = now;
}


void ReliableLink::fail()
{
	NETLINK_LOG_WARNING("Link failed: the remote did not answer for {} ms", mTimings.peerTimeout.count());

	resetStreams();
	mLastReceived.reset();
	mFailed = true;
}


// ---------------------------------------------------------------------------
// Timers
// ---------------------------------------------------------------------------

void ReliableLink::onTimer(const TimePoint now)
{
	if (mFailed)
		return;

	if ((mStalledSince && now - *mStalledSince >= mTimings.peerTimeout) || (mLastReceived && now - *mLastReceived >= mTimings.peerTimeout))
	{
		fail();
		return;
	}

	// A remote whose stream is not known yet cannot be asked: nothing but the first Control packet goes out without it
	if (mLastReceived && mRemoteStreamID && now - std::max(*mLastReceived, mPingedAt) >= mTimings.keepAlive)
	{
		mPingDue |= bitOf(Lane::Control);
		mPingedAt = now;
	}

	// The timeout backs off while nothing at all is acknowledged, not per packet: on a path that merely loses packets,
	// one that was unlucky twice is not sent any later for it
	const auto timeout = mRtt.timeoutFor(mTimeoutsInARow);
	bool	   expired = false;

	while (!mSent.empty())
	{
		const SentRecord record = mSent.front();
		InFlight		*entry	= entryOf(record);
		++mStats.timerWork;

		if (entry && now < entry->sentAt + timeout)
			break;

		mSent.pop_front();

		if (entry)
		{
			markLost(record, *existingStream(record.lane), *entry, true);
			expired = true;
		}
	}

	if (expired && mTimeoutsInARow < MaxBackoffSteps)
		++mTimeoutsInARow;

	for (const Lane lane : StreamLanes)
	{
		Stream *stream = existingStream(lane);

		if (stream && stream->peerPaused && stream->probeAt && now >= *stream->probeAt)
		{
			mPingDue |= bitOf(lane);
			stream->probeInterval = std::min(stream->probeInterval * 2, mTimings.maxRto);
			stream->probeAt		  = now + stream->probeInterval;
		}
	}
}


std::optional<ReliableLink::TimePoint> ReliableLink::nextDeadline() const
{
	std::optional<TimePoint> next;

	const auto				 consider = [&next](const TimePoint when)
	{
		if (!next || when < *next)
			next = when;
	};

	if (!mSent.empty())
	{
		const SentRecord &oldest = mSent.front();
		const Stream	 *stream = existingStream(oldest.lane);

		if (const InFlight *entry = stream ? stream->inFlight.find(oldest.seq) : nullptr)
			consider(entry->sentAt + mRtt.timeoutFor(mTimeoutsInARow));
	}

	if (mStalledSince)
		consider(*mStalledSince + mTimings.peerTimeout);

	if (mLastReceived)
	{
		consider(*mLastReceived + mTimings.peerTimeout);

		if (mRemoteStreamID)
			consider(std::max(*mLastReceived, mPingedAt) + mTimings.keepAlive);
	}

	for (const auto &stream : mStreams)
	{
		if (stream && stream->peerPaused && stream->probeAt)
			consider(*stream->probeAt);
	}

	return next;
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
	OutgoingDatagram datagram;
	datagram.headSize = static_cast<uint8_t>(encodeHeader(header, datagram.head.data()));
	return datagram;
}


bool ReliableLink::hasPendingReliable() const
{
	return std::ranges::any_of(mStreams, [](const auto &stream) { return stream && (!stream->inFlight.empty() || stream->cursor.has_value()); });
}


bool ReliableLink::hasPending(const Lane lane) const
{
	if (lane == Lane::Media)
		return mMediaCursor.has_value();

	const Stream *stream = existingStream(lane);
	return stream && (!stream->inFlight.empty() || stream->cursor.has_value());
}


size_t ReliableLink::inFlightCount() const
{
	size_t count = 0;
	for (const auto &stream : mStreams)
		count += stream ? stream->inFlight.size() : 0;
	return count;
}


void ReliableLink::resetStreams()
{
	// Nothing of the old streams may still go out, and the new ones learn the connection quality anew
	for (auto &stream : mStreams)
		stream.reset();

	mMediaCursor.reset();
	mMediaAssembler.reset();
	mNextMediaSeq  = 1;
	mMediaReceived = 0;

	mRtt.reset();
	mSent.clear();
	mCongestionWindow = initialWindow(mTimings);
	mTransmissions	  = 0;
	mLargestAcked	  = 0;
	mRecoveryStart	  = 0;
	mTimeoutsInARow	  = 0;
	mStalledSince.reset();

	mPendingAcks.clear();
	mAckDue	 = 0;
	mPingDue = 0;
}

} // namespace netlink::channel
