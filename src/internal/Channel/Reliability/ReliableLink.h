/*
  ==============================================================================
	Module:         ReliableLink
	Description:    Reliable, ordered message streams to one remote peer on top
					of datagrams (Data -> DataAck -> AckAck per seq)
  ==============================================================================
*/

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "Channel/Fragmentation/MessageAssembler.h"
#include "Channel/Protocol/AckRanges.h"
#include "Channel/Protocol/PacketHeader.h"
#include "Channel/Queue/BoundedQueue.h"
#include "Channel/Queue/SequenceBuffer.h"
#include "ReliabilityConfig.h"
#include "RttEstimator.h"


/*
 Reliable flow:
	Every reliable packet is identified by its 64-bit seq (unique within the sender's current stream and channel) and
	completes a three-way exchange:

		Data(seq) -> DataAck(seq) -> AckAck(seq)

	The sender retransmits Data until the DataAck arrives, the receiver retransmits the DataAck until the AckAck arrives.
	One DataAck or AckAck datagram carries the seqs of everything that arrived since the last one, so the exchange costs
	two small datagrams per batch of Data packets instead of two per packet.

 Channels:
	Control and Application are streams of their own (seqs, send window, reorder buffer), sharing only the congestion
	window. Application data can neither delay a control signal nor hold it back in the receiver's reorder buffer.

 Loss and congestion:
	A packet is retransmitted as soon as packets sent after it are acknowledged (fast retransmit), otherwise after its
	retransmission timeout. How many packets may be unacknowledged at once is adapted to the path (congestion window).
 */


namespace netlink::channel
{

using MessageBody = std::shared_ptr<const std::vector<uint8_t>>;


// A whole message waiting to be sent reliably
struct OutboundMessage
{
	ChannelId	channel{ChannelId::Control};
	uint32_t	tag{0};
	MessageBody body;
};

// A message as it was sent
struct DeliveredMessage
{
	ChannelId			 channel{ChannelId::Control};
	uint32_t			 tag{0};
	std::vector<uint8_t> body;
};

// One datagram for the socket
struct OutgoingDatagram
{
	std::array<uint8_t, MaxHeaderSize> head{};
	uint8_t							   headSize{0};
	MessageBody						   message; // reliable data: keeps the message alive until the datagram was sent
	uint32_t						   offset{0};
	uint32_t						   length{0};
	std::vector<uint8_t>			   owned;	// everything else: the body itself

	std::span<const uint8_t>		   header() const { return {head.data(), headSize}; }
	std::span<const uint8_t>		   body() const { return message ? std::span<const uint8_t>(message->data() + offset, length) : std::span<const uint8_t>(owned); }

	// Header and body joined, as they go onto the wire
	std::vector<uint8_t>			   bytes() const;
};

enum class LinkEvent
{
	Failed,		   // Data stayed unacknowledged for too long: the link restarted its stream
	PeerRestarted, // The remote came back with a new stream ID: all state for it was reset
};

struct LinkStats
{
	uint64_t dataSent{0};			 // Data packets, first transmissions
	uint64_t retransmissions{0};	 // Data packets sent again, for whatever reason
	uint64_t fastRetransmissions{0}; // ... of these: detected by later acknowledgements instead of the timeout
	uint64_t dataAcksSent{0};		 // DataAck datagrams
	uint64_t ackAcksSent{0};		 // AckAck datagrams
	uint64_t duplicatesReceived{0};
	uint64_t staleDropped{0};
	uint64_t outOfWindowDropped{0};
	uint64_t delivered{0}; // whole messages
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

	// Queues a whole message; it is fragmented and sent as the windows allow. Rejected when too large or the queue is full.
	PushResult					  queueReliable(ChannelId channel, uint32_t tag, std::vector<uint8_t> body);

	// Sent with the next send pass, without acknowledgement. False when the body does not fit into one datagram.
	bool						  sendUnreliable(ChannelId channel, uint32_t tag, std::span<const uint8_t> body);

	void						  sendHeartbeat();

	// --- Receiving -------------------------------------------------------------

	void						  onPacket(const DecodedPacket &packet, TimePoint now);

	// Whether the application takes its messages as fast as they arrive. While it does not, the remote is told to pause
	// the application channel instead of piling up more.
	void						  setApplicationReceiving(const bool receiving) { mApplicationReceiving = receiving; }

	// --- Timers ----------------------------------------------------------------

	// Declares overdue Data packets lost, lists unconfirmed DataAcks again and detects a failed link
	void						  onTimer(TimePoint now);
	std::optional<TimePoint>	  nextDeadline() const { return mNextDeadline; }

	// --- Output ----------------------------------------------------------------

	// One send pass: acknowledgements and the heartbeat first, then unreliable data, then as much reliable data as the windows allow
	std::vector<OutgoingDatagram> takeOutgoing(TimePoint now);
	std::vector<DeliveredMessage> takeDelivered() { return std::exchange(mDelivered, {}); }
	std::vector<LinkEvent>		  takeEvents() { return std::exchange(mEvents, {}); }

	// --- State -----------------------------------------------------------------

	// A reliable message of that channel would be queued right now (always the case under OverflowPolicy::DropOldest)
	bool						  hasRoomFor(ChannelId channel) const;

	// Something reliable is still unacknowledged or waiting to be sent
	bool						  hasPendingReliable() const;

	// The next send pass would produce datagrams
	bool						  hasOutgoing() const;

	// Data packets that were sent and are not acknowledged yet (those waiting for their retransmission included)
	size_t						  inFlightCount() const;
	size_t						  queuedMessageCount() const;
	size_t						  congestionWindow() const { return static_cast<size_t>(mCongestionWindow); }

	// Forgets application messages that were not put on the wire yet (session closed)
	void						  dropQueuedApplicationMessages();

	uint32_t					  localStreamID() const { return mLocalStreamID; }
	std::optional<uint32_t>		  remoteStreamID() const { return mRemoteStreamID; }
	const RttEstimator			 &rtt() const { return mRtt; }
	const LinkStats				 &stats() const { return mStats; }

	// Largest body of one fragment, and of one unreliable message
	size_t						  maxFragmentBody() const;
	size_t						  maxUnreliableBody() const;

private:
	static constexpr size_t	   ChannelCount				  = 2;
	static constexpr ChannelId ChannelOrder[ChannelCount] = {ChannelId::Control, ChannelId::Application}; // control always goes first

	struct InFlight
	{
		PacketHeader header;		  // stream IDs are stamped with every transmission
		MessageBody	 message;		  // the fragment is message[offset, offset + length)
		uint32_t	 offset{0};
		uint32_t	 length{0};
		TimePoint	 sentAt{};		  // latest transmission
		TimePoint	 deadline{};	  // ... and when it counts as lost without an acknowledgement
		uint64_t	 transmission{0}; // position of the latest transmission among everything this link sent
		int			 transmissions{0};
		bool		 lost{false};	  // waiting for its retransmission: not on the wire, no timer running
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

	// Everything one channel needs for its reliable stream, in both directions
	struct Stream
	{
		Stream(const size_t queueCapacity, const OverflowPolicy overflow, const size_t maxMessageSize) : queue(queueCapacity, overflow), assembler(maxMessageSize) {}

		// Send side
		BoundedQueue<OutboundMessage>			 queue;
		std::optional<FragmentCursor>			 cursor;
		SequenceBuffer<InFlight, WindowSize>	 inFlight;
		std::deque<uint64_t>					 lost;			  // seqs waiting for their retransmission, the most urgent first
		uint64_t								 nextSendSeq{1};
		uint64_t								 sendBase{1};	  // lowest unacknowledged seq
		uint64_t								 highestAcked{0}; // highest seq the remote acknowledged
		size_t									 onTheWire{0};	  // sent and neither acknowledged nor considered lost
		size_t									 peerWindow{WindowSize};
		std::optional<TimePoint>				 probeAt;		  // peer window closed: when the next packet may ask again
		std::vector<SeqRange>					 ackAcks;		  // DataAcks to confirm with the next send pass
		bool									 ackAckDue{false};

		// Receive side
		SequenceBuffer<BufferedData, WindowSize> reorder;
		SequenceBuffer<AckRecord, WindowSize>	 ackRecords; // DataAcks the remote did not confirm yet
		uint64_t								 nextExpected{1};
		uint64_t								 ackAckedThrough{0};
		std::vector<uint64_t>					 dataAcks;	 // seqs to acknowledge with the next send pass
		MessageAssembler						 assembler;
	};

	static size_t									  indexOf(const ChannelId channel) { return channel == ChannelId::Control ? 0 : 1; }

	// Streams are created with their first use: most links only ever carry control signals
	Stream											 &streamFor(ChannelId channel);
	Stream											 *existingStream(const ChannelId channel) { return mStreams[indexOf(channel)].get(); }
	const Stream									 *existingStream(const ChannelId channel) const { return mStreams[indexOf(channel)].get(); }

	void											  handleReliableData(ChannelId channel, const PacketHeader &header, std::span<const uint8_t> body, TimePoint now);
	void											  handleUnreliableData(const PacketHeader &header, std::span<const uint8_t> body);
	void											  handleDataAck(Stream &stream, const PacketHeader &header, std::span<const uint8_t> body, TimePoint now);
	static void										  handleAckAck(Stream &stream, const PacketHeader &header, std::span<const uint8_t> body);

	// Hands a packet that is next in its stream to the assembler
	void											  acceptInOrder(Stream &stream, ChannelId channel, const PacketHeader &header, std::span<const uint8_t> body);

	// Send pass
	void											  flushAcks(Stream &stream, ChannelId channel, std::vector<OutgoingDatagram> &pass);
	void											  sendData(Stream &stream, ChannelId channel, std::vector<OutgoingDatagram> &pass, TimePoint now);
	bool											  mayTransmit(Stream &stream, TimePoint now);
	static InFlight									 *nextLost(Stream &stream);
	InFlight										 *nextFragment(Stream &stream, ChannelId channel) const;
	void											  transmit(Stream &stream, InFlight &entry, std::vector<OutgoingDatagram> &pass, TimePoint now);
	static bool										  hasSendable(const Stream &stream);

	// Loss and congestion
	bool											  detectLosses(Stream &stream, TimePoint now);
	void											  markLost(Stream &stream, uint64_t seq, InFlight &entry, bool timedOut);
	void											  growCongestionWindow(size_t acknowledged);
	static void										  advanceSendBase(Stream &stream);
	void											  fail();

	// Encodes with the current stream IDs: they may have become known since the packet was created
	OutgoingDatagram								  makeDatagram(const PacketHeader &header) const;
	OutgoingDatagram								  makeDatagram(const PacketHeader &header, std::span<const uint8_t> body) const;
	PacketHeader									  makeHeader(PacketFlags flags, uint64_t seq) const;
	void											  scheduleDeadline(TimePoint deadline);
	size_t											  maxRangesPerDatagram() const;

	// Starts fresh streams: used after a failure (new local stream ID) and after a peer restart
	void											  resetStreams();


	ReliabilityConfig								  mConfig;
	uint32_t										  mLocalStreamID;
	std::optional<uint32_t>							  mRemoteStreamID;
	RttEstimator									  mRtt;

	std::array<std::unique_ptr<Stream>, ChannelCount> mStreams;

	// Congestion control, shared by the channels: they travel the same path
	double											  mCongestionWindow;
	double											  mSlowStartThreshold;
	size_t											  mOnTheWire{0};		 // Data packets sent and neither acknowledged nor considered lost
	uint64_t										  mTransmissions{0};	 // counts every Data transmission
	uint64_t										  mLargestAcked{0};		 // the latest transmission that was acknowledged
	uint64_t										  mRecoveryStart{0};	 // losses of transmissions up to this one already reduced the window
	bool											  mWindowLimited{false}; // the last send pass had more to send than the window allowed
	std::vector<uint64_t>							  mAcknowledged;		 // scratch: the transmissions one DataAck acknowledged
	std::optional<TimePoint>						  mStalledSince;		 // data is unacknowledged: when the last acknowledgement arrived
	int												  mTimeoutsInARow{0};	 // retransmission timeouts since the last acknowledgement

	bool											  mApplicationReceiving{true};

	// Unreliable data and the heartbeat
	uint64_t										  mNextUnreliableSeq{1};
	uint64_t										  mLastUnreliableSeq{0};
	BoundedQueue<OutgoingDatagram>					  mUnreliableQueue; // the oldest dropped when full
	bool											  mHeartbeatDue{false};

	std::optional<TimePoint>						  mNextDeadline;

	std::vector<DeliveredMessage>					  mDelivered;
	std::vector<LinkEvent>							  mEvents;
	LinkStats										  mStats;
};

} // namespace netlink::channel
