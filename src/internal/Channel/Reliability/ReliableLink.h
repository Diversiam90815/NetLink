/*
  ==============================================================================
	Module:         ReliableLink
	Description:    Reliable, ordered message stream to one remote peer on top
					of datagrams (Data -> DataAck -> AckAck per seq)
  ==============================================================================
*/

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "Channel/Protocol/PacketHeader.h"
#include "Channel/Queue/BoundedQueue.h"
#include "Channel/Queue/SequenceBuffer.h"
#include "ReliabilityConfig.h"
#include "RttEstimator.h"


/*
 Reliable flow:
	Every reliable packet is identified by its 64-bit seq (unique within the sender's current stream) and
	completes a three-way exchange:

		Data(seq) -> DataAck(seq) -> AckAck(seq)

	The sender retransmits Data until the DataAck arrives, the receiver retransmits the DataAck until the AckAck arrives.
 */


namespace netlink::channel
{

// A whole message waiting to be sent reliably
struct OutboundMessage
{
	ChannelId			 channel{ChannelId::Control};
	std::vector<uint8_t> body;
};

// A Data packet accepted by the link: reliable ones in seq order, fragments are not reassembled here
struct DeliveredPacket
{
	PacketHeader		 header;
	std::vector<uint8_t> body;
};

enum class LinkEvent
{
	Failed,		   // A packet exhausted its retransmissions: the link restarted its stream
	PeerRestarted, // The remote came back with a new stream ID: all state for it was reset
};

struct LinkStats
{
	uint64_t dataSent{0};
	uint64_t retransmissions{0};
	uint64_t dataAcksSent{0};
	uint64_t ackAcksSent{0};
	uint64_t duplicatesReceived{0};
	uint64_t staleDropped{0};
	uint64_t outOfWindowDropped{0};
	uint64_t delivered{0};
};

// Random, non-zero stream ID. Identifies one lifetime of a link's stream, so a peer can tell a fresh stream (seq restarts at 1) from a stale one.
uint32_t makeStreamID(uint32_t different = 0);


class ReliableLink
{
public:
	using Clock									 = std::chrono::steady_clock;
	using TimePoint								 = Clock::time_point;

	// Control messages are never subject to the application's queue policy
	static constexpr size_t ControlQueueCapacity = 512;

	explicit ReliableLink(const ReliabilityConfig &config = {}, uint32_t localStreamID = makeStreamID());

	// --- Sending --------------------------------------------------------------

	// Queues a whole message; it is fragmented and sent as the send window allows. Rejected when too large or the queue is full.
	PushResult						  queueReliable(ChannelId channel, std::vector<uint8_t> body);

	// Sent with the next send pass, without acknowledgement. False when the body does not fit into one datagram.
	bool							  sendUnreliable(ChannelId channel, std::span<const uint8_t> body);

	void							  sendHeartbeat();

	// --- Receiving -------------------------------------------------------------

	void							  onPacket(const DecodedPacket &packet, TimePoint now);

	// --- Timers ----------------------------------------------------------------

	// Retransmits due Data and DataAck packets
	void							  onTimer(TimePoint now);
	std::optional<TimePoint>		  nextDeadline() const { return mNextDeadline; }

	// --- Output ----------------------------------------------------------------

	// One send pass: at most sendBudget() datagrams for the socket, signals first, then unreliable, then reliable data.
	// The rest waits for the next pass. Reliable fragments start their retransmission timer now.
	std::vector<std::vector<uint8_t>> takeOutgoing(TimePoint now);
	std::vector<DeliveredPacket>	  takeDelivered() { return std::exchange(mDelivered, {}); }
	std::vector<LinkEvent>			  takeEvents() { return std::exchange(mEvents, {}); }

	// --- State -----------------------------------------------------------------

	// Something reliable is still in flight or waiting to be sent
	bool							  hasPendingReliable() const;

	// Datagrams are waiting for a send pass
	bool							  hasOutgoing() const;
	size_t							  sendBudget() const { return mSendBudget; }
	size_t							  inFlightCount() const { return mInFlight.size(); }
	size_t							  queuedMessageCount() const;

	// Forgets application messages that were not put on the wire yet (session closed)
	void							  dropQueuedApplicationMessages();

	uint32_t						  localStreamID() const { return mLocalStreamID; }
	std::optional<uint32_t>			  remoteStreamID() const { return mRemoteStreamID; }
	const RttEstimator				 &rtt() const { return mRtt; }
	const LinkStats					 &stats() const { return mStats; }

	// Largest body of one fragment, and of one unreliable message
	size_t							  maxFragmentBody() const;
	size_t							  maxUnreliableBody() const;

private:
	struct InFlight
	{
		PacketHeader		 header;
		std::vector<uint8_t> body;
		TimePoint			 firstSent{}; // set by the first send pass that pushes it
		TimePoint			 deadline{};
		int					 transmissions{0};
		bool				 queued{false}; // waiting in mDataQueue for a send pass; its timer is not running
	};

	struct AckRecord
	{
		TimePoint deadline{};
		int		  retransmits{0};
	};

	struct BufferedData
	{
		PacketHeader		 header;
		std::vector<uint8_t> body;
	};

	// Message currently being cut into fragments
	struct FragmentCursor
	{
		OutboundMessage message;
		size_t			count{0};
		size_t			next{0};
	};

	void									 handleReliableData(const PacketHeader &header, std::span<const uint8_t> body, TimePoint now);
	void									 handleUnreliableData(const PacketHeader &header, std::span<const uint8_t> body);
	void									 handleDataAck(const PacketHeader &header, TimePoint now);

	// Moves queued fragments into the send window while there is room
	void									 pump();
	std::optional<OutboundMessage>			 nextQueuedMessage();
	void									 advanceSendBase();

	// The budget of the coming send pass, from what the link learned since the last one
	void									 adaptSendBudget();

	// Encodes with the current stream IDs: they may have become known since the packet was created
	std::vector<uint8_t>					 encode(const PacketHeader &header, std::span<const uint8_t> body) const;
	void									 sendAck(PacketKind kind, uint64_t seq);
	PacketHeader							 makeHeader(PacketFlags flags, uint64_t seq) const;
	void									 scheduleDeadline(TimePoint deadline);

	// Starts a fresh stream: used after a failure (new local stream ID) and after a peer restart
	void									 resetSendState();
	void									 resetReceiveState();


	ReliabilityConfig						 mConfig;
	uint32_t								 mLocalStreamID;
	std::optional<uint32_t>					 mRemoteStreamID;
	RttEstimator							 mRtt;

	// Send side
	BoundedQueue<OutboundMessage>			 mControlQueue;
	BoundedQueue<OutboundMessage>			 mApplicationQueue;
	std::optional<FragmentCursor>			 mCursor;
	SequenceBuffer<InFlight, WindowSize>	 mInFlight;
	uint64_t								 mNextSendSeq{1};
	uint64_t								 mSendBase{1}; // lowest unacknowledged seq
	uint64_t								 mNextUnreliableSeq{1};

	// Receive side
	SequenceBuffer<BufferedData, WindowSize> mReorder;
	SequenceBuffer<AckRecord, WindowSize>	 mAckRecords;
	uint64_t								 mNextExpected{1};
	uint64_t								 mLastUnreliableSeq{0};

	std::optional<TimePoint>				 mNextDeadline;

	// Output stage, drained by send passes in this order
	BoundedQueue<std::vector<uint8_t>>		 mSignalQueue;	   // DataAck, AckAck, heartbeat
	BoundedQueue<std::vector<uint8_t>>		 mUnreliableQueue; // unreliable Data, the oldest dropped when full
	BoundedQueue<uint64_t>					 mDataQueue;	   // seqs of reliable fragments due for (re)transmission
	size_t									 mSendBudget;
	bool									 mLossSinceLastPass{false};
	bool									 mLastPassFull{false};

	std::vector<DeliveredPacket>			 mDelivered;
	std::vector<LinkEvent>					 mEvents;
	LinkStats								 mStats;
};

} // namespace netlink::channel
