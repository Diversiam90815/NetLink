/*
  ==============================================================================
	Module:         ReliableLink
	Description:    Reliable, ordered message stream to one remote peer on top
					of datagrams (Data -> DataAck -> AckAck per message key)
  ==============================================================================
*/

#include "ReliableLink.h"

#include <random>

#include "Channel/Fragmentation/FragmentationService.h"
#include "NetLinkLog.h"


namespace netlink::channel
{

uint32_t makeStreamID(const uint32_t different)
{
	thread_local std::mt19937				generator{std::random_device{}()};
	std::uniform_int_distribution<uint32_t> distribution(1, UINT32_MAX);

	uint32_t								id = 0;
	do
	{
		id = distribution(generator);
	} while (id == different);

	return id;
}


ReliableLink::ReliableLink(const ReliabilityConfig &config, const uint32_t localStreamID)
	: mConfig(config), mLocalStreamID(localStreamID != 0 ? localStreamID : makeStreamID()), mRtt(config.initialRto, config.minRto, config.maxRto),
	  mControlQueue(ControlQueueCapacity, OverflowPolicy::DropNewest), mApplicationQueue(config.sendQueueCapacity, config.sendQueueOverflow)
{
}


size_t ReliableLink::maxFragmentBody() const
{
	constexpr size_t overhead = BaseHeaderSize + FragmentExtensionSize;
	return mConfig.maxDatagramSize > overhead ? mConfig.maxDatagramSize - overhead : 0;
}


size_t ReliableLink::maxUnreliableBody() const
{
	return mConfig.maxDatagramSize > BaseHeaderSize ? mConfig.maxDatagramSize - BaseHeaderSize : 0;
}


// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

PushResult ReliableLink::queueReliable(const ChannelId channel, std::vector<uint8_t> body, const TimePoint now)
{
	if (body.size() > mConfig.maxMessageSize || FragmentationService::fragmentCount(body.size(), maxFragmentBody()) == 0)
	{
		NETLINK_LOG_ERROR("Message of {} bytes exceeds the maximum message size", body.size());
		return PushResult::Rejected;
	}

	auto						  &queue = channel == ChannelId::Control ? mControlQueue : mApplicationQueue;
	std::optional<OutboundMessage> evicted;
	const PushResult			   result = queue.push(OutboundMessage{.channel = channel, .body = std::move(body)}, &evicted);

	if (result == PushResult::Rejected)
		NETLINK_LOG_WARNING("Send queue full ({} messages), message rejected", queue.capacity());
	else if (result == PushResult::EvictedOldest)
		NETLINK_LOG_WARNING("Send queue full ({} messages), dropped the oldest unsent message", queue.capacity());

	pump(now);
	return result;
}


bool ReliableLink::sendUnreliable(const ChannelId channel, const std::span<const uint8_t> body)
{
	if (body.size() > maxUnreliableBody())
		return false;

	transmit(makeHeader(PacketFlags::data(channel, false), mNextUnreliableSeq++), body);
	return true;
}


void ReliableLink::sendHeartbeat()
{
	transmit(makeHeader(PacketFlags::heartbeat(), 0), {});
}


std::optional<OutboundMessage> ReliableLink::nextQueuedMessage()
{
	// Connection and validation signals go first, so application traffic cannot hold them up
	if (auto control = mControlQueue.pop())
		return control;

	return mApplicationQueue.pop();
}


void ReliableLink::pump(const TimePoint now)
{
	// All in-flight seqs stay within one window, which keeps the SequenceBuffer slots unique
	while (mNextSendSeq - mSendBase < WindowSize)
	{
		if (!mCursor)
		{
			auto message = nextQueuedMessage();
			if (!message)
				return;

			const size_t count = FragmentationService::fragmentCount(message->body.size(), maxFragmentBody());
			mCursor			   = FragmentCursor{.message = std::move(*message), .count = count, .next = 0};
		}

		const Fragment fragment = FragmentationService::fragmentAt(mCursor->message.body, mCursor->next, maxFragmentBody());

		PacketHeader   header	= makeHeader(PacketFlags::data(mCursor->message.channel, true), mNextSendSeq++);
		if (fragment.isFragmented())
		{
			header.flags.setFragment(true, fragment.isLast());
			header.fragIndex = fragment.index;
			header.fragCount = fragment.count;
		}

		InFlight entry;
		entry.header		= header;
		entry.body			= std::vector<uint8_t>(fragment.body.begin(), fragment.body.end());
		entry.firstSent		= now;
		entry.deadline		= now + mRtt.timeoutFor(0);
		entry.transmissions = 1;

		transmit(entry.header, entry.body);
		++mStats.dataSent;
		scheduleDeadline(entry.deadline);
		mInFlight.insert(header.seq, std::move(entry));

		if (++mCursor->next >= mCursor->count)
			mCursor.reset();
	}
}


void ReliableLink::advanceSendBase()
{
	while (mSendBase < mNextSendSeq && !mInFlight.contains(mSendBase))
		++mSendBase;
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
		resetSendState();
		resetReceiveState();
		mRemoteStreamID = header.srcStreamID;
		mEvents.push_back(LinkEvent::PeerRestarted);
	}

	switch (header.flags.kind())
	{
	case PacketKind::Data:
		if (header.flags.isReliable())
			handleReliableData(header, packet.body, now);
		else
			handleUnreliableData(header, packet.body);
		break;

	case PacketKind::DataAck: handleDataAck(header, now); break;

	case PacketKind::AckAck: mAckRecords.erase(header.seq); break;

	case PacketKind::Heartbeat: break; // Liveness only, tracked by the owner
	}
}


void ReliableLink::handleReliableData(const PacketHeader &header, std::span<const uint8_t> body, const TimePoint now)
{
	const uint64_t seq = header.seq;

	if (seq == 0)
		return;

	// No room to buffer it: without an ack the sender retransmits once the window moved on
	if (seq >= mNextExpected + WindowSize)
	{
		++mStats.outOfWindowDropped;
		return;
	}

	// Every Data packet is acknowledged, duplicates too: the earlier DataAck may have been lost
	sendAck(PacketKind::DataAck, seq);

	if (seq < mNextExpected || mReorder.contains(seq))
	{
		++mStats.duplicatesReceived;
		return;
	}

	AckRecord record;
	record.deadline = now + mRtt.timeoutFor(0);
	mAckRecords.insert(seq, record);
	scheduleDeadline(record.deadline);

	mReorder.insert(seq, BufferedData{.header = header, .body = std::vector<uint8_t>(body.begin(), body.end())});

	while (auto ready = mReorder.take(mNextExpected))
	{
		mDelivered.push_back(DeliveredPacket{.header = ready->header, .body = std::move(ready->body)});
		++mStats.delivered;
		++mNextExpected;
	}
}


void ReliableLink::handleUnreliableData(const PacketHeader &header, std::span<const uint8_t> body)
{
	// Sequenced: anything older than what was already delivered is stale
	if (header.seq <= mLastUnreliableSeq)
	{
		++mStats.staleDropped;
		return;
	}

	mLastUnreliableSeq = header.seq;
	mDelivered.push_back(DeliveredPacket{.header = header, .body = std::vector<uint8_t>(body.begin(), body.end())});
	++mStats.delivered;
}


void ReliableLink::handleDataAck(const PacketHeader &header, const TimePoint now)
{
	const uint64_t seq = header.seq;

	// Never sent by us
	if (seq == 0 || seq >= mNextSendSeq)
		return;

	// Always confirmed, also for a key already completed: the receiver missed the earlier AckAck
	sendAck(PacketKind::AckAck, seq);

	const auto entry = mInFlight.take(seq);
	if (!entry)
		return;

	// Karn: only unambiguous samples
	if (entry->transmissions == 1)
		mRtt.addSample(std::chrono::duration_cast<RttEstimator::Duration>(now - entry->firstSent));

	advanceSendBase();
	pump(now);
}


// ---------------------------------------------------------------------------
// Timers
// ---------------------------------------------------------------------------

void ReliableLink::onTimer(const TimePoint now)
{
	if (!mNextDeadline || now < *mNextDeadline)
		return;

	mNextDeadline.reset();
	bool failed = false;

	mInFlight.forEach(
		[&](uint64_t, InFlight &entry)
		{
			if (failed)
				return true;

			if (now >= entry.deadline)
			{
				if (entry.transmissions > mConfig.maxRetransmits)
				{
					failed = true;
					return true;
				}

				entry.deadline = now + mRtt.timeoutFor(entry.transmissions);
				++entry.transmissions;
				++mStats.retransmissions;
				transmit(entry.header, entry.body);
			}

			scheduleDeadline(entry.deadline);
			return true;
		});

	if (failed)
	{
		NETLINK_LOG_WARNING("Link failed: a packet was not acknowledged after {} retransmissions", mConfig.maxRetransmits);

		// The stream cannot continue with a gap: start a new one under a new stream ID, which tells the remote to reset as well
		mLocalStreamID = makeStreamID(mLocalStreamID);
		mRemoteStreamID.reset();
		resetSendState();
		resetReceiveState();
		mEvents.push_back(LinkEvent::Failed);
		return;
	}

	mAckRecords.forEach(
		[&](const uint64_t seq, AckRecord &record)
		{
			if (now >= record.deadline)
			{
				// Best effort: the sender's Data retransmission triggers a fresh DataAck anyway
				if (record.retransmits >= mConfig.maxAckRetransmits)
					return false;

				++record.retransmits;
				record.deadline = now + mRtt.timeoutFor(record.retransmits);
				sendAck(PacketKind::DataAck, seq);
			}

			scheduleDeadline(record.deadline);
			return true;
		});
}


void ReliableLink::scheduleDeadline(TimePoint deadline)
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


void ReliableLink::transmit(const PacketHeader &header, const std::span<const uint8_t> body)
{
	// Re-stamped on every transmission: stream IDs may have become known since the first one
	PacketHeader stamped = header;
	stamped.srcStreamID	 = mLocalStreamID;
	stamped.dstStreamID	 = mRemoteStreamID.value_or(0);

	mOutgoing.push_back(encodePacket(stamped, body));
}


void ReliableLink::sendAck(const PacketKind kind, const uint64_t seq)
{
	transmit(makeHeader(PacketFlags::ack(kind), seq), {});

	if (kind == PacketKind::DataAck)
		++mStats.dataAcksSent;
	else
		++mStats.ackAcksSent;
}


bool ReliableLink::hasPendingReliable() const
{
	return !mInFlight.empty() || mCursor.has_value() || !mControlQueue.empty() || !mApplicationQueue.empty();
}


size_t ReliableLink::queuedMessageCount() const
{
	return mControlQueue.size() + mApplicationQueue.size() + (mCursor ? 1 : 0);
}


void ReliableLink::dropQueuedApplicationMessages()
{
	mApplicationQueue.clear();

	// A message that is partly on the wire cannot be completed anymore either; the receiver abandons the partial message
	if (mCursor && mCursor->message.channel == ChannelId::Application)
		mCursor.reset();
}


void ReliableLink::resetSendState()
{
	mControlQueue.clear();
	mApplicationQueue.clear();
	mCursor.reset();
	mInFlight.clear();
	mNextSendSeq	   = 1;
	mSendBase		   = 1;
	mNextUnreliableSeq = 1;
	mRtt.reset();
}


void ReliableLink::resetReceiveState()
{
	mReorder.clear();
	mAckRecords.clear();
	mNextExpected	   = 1;
	mLastUnreliableSeq = 0;
	mNextDeadline.reset();
}

} // namespace netlink::channel
