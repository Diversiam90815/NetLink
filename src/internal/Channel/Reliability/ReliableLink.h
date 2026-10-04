/*
  ==============================================================================
	Module:         ReliableLink
	Description:    Message streams to one remote peer on top of datagrams:
					three acknowledged, ordered lanes and one that is neither
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

#include "Channel/Fragmentation/MediaAssembler.h"
#include "Channel/Fragmentation/MessageAssembler.h"
#include "Channel/Protocol/AckRanges.h"
#include "Channel/Protocol/PacketHeader.h"
#include "Channel/Queue/SequenceBuffer.h"
#include "RttEstimator.h"
#include "TransportConstants.h"


/*
 Lanes:
	Control, Reliable and Bulk are streams of their own (seqs, send window, reorder buffer): what is lost on one of them
	holds nothing back on the others. Every Data packet has a 64-bit seq; the receiver answers with an Ack that names
	the highest seq it has without a gap, and what it holds behind one. Media messages are sent once and never
	acknowledged.

 Loss and congestion:
	A packet is sent again as soon as packets sent after it are acknowledged (fast retransmit), otherwise after its
	retransmission timeout. How many Reliable and Bulk packets may be unacknowledged at once is adapted to the path
	(congestion window). Control packets do not wait for that window.

 Pausing:
	A receiver whose application does not keep up, or that has no room for another large message, sets the pause bit
	in the Acks of a lane. The sender then holds that lane back and asks again with a Ping from time to time.
 */


namespace netlink::channel
{

using MessageBody = std::shared_ptr<const std::vector<uint8_t>>;


// A whole message to be sent
struct OutboundMessage
{
	uint32_t	tag{0};
	MessageBody body;
};

// A message as it was sent
struct DeliveredMessage
{
	Lane				 lane{Lane::Control};
	uint32_t			 tag{0};
	std::vector<uint8_t> body;
};

// One datagram for the socket
struct OutgoingDatagram
{
	std::array<uint8_t, MaxHeaderSize> head{};
	uint8_t							   headSize{0};
	MessageBody						   message; // data: keeps the message alive until the datagram was sent
	uint32_t						   offset{0};
	uint32_t						   length{0};
	std::vector<uint8_t>			   owned;	// everything else: the body itself

	std::span<const uint8_t>		   header() const { return {head.data(), headSize}; }
	std::span<const uint8_t>		   body() const { return message ? std::span<const uint8_t>(message->data() + offset, length) : std::span<const uint8_t>(owned); }

	// Header and body joined, as they go onto the wire
	std::vector<uint8_t>			   bytes() const;
};

// Where a link takes the messages of a lane from, one at a time, when it is able to send the next one
class MessageSource
{
public:
	virtual ~MessageSource()							   = default;
	virtual std::optional<OutboundMessage> next(Lane lane) = 0;
};

struct LinkStats
{
	uint64_t dataSent{0};			 // Data packets of the acknowledged lanes, first transmissions
	uint64_t retransmissions{0};	 // ... sent again, for whatever reason
	uint64_t fastRetransmissions{0}; // ... of these: detected by later acknowledgements instead of the timeout
	uint64_t acksSent{0};
	uint64_t pingsSent{0};
	uint64_t duplicatesReceived{0};
	uint64_t staleDropped{0};
	uint64_t outOfWindowDropped{0};
	uint64_t delivered{0};			 // whole messages
	uint64_t bytesSent{0};			 // payload that was sent for the first time, Control aside
	uint64_t bytesReceived{0};		 // payload of the delivered messages, Control aside
	uint64_t mediaSent{0};			 // Media datagrams
	uint64_t mediaReceived{0};		 // ... that arrived here
	uint64_t mediaReceivedByPeer{0}; // ... that arrived at the remote, as its latest Ack said
	uint64_t timerWork{0};			 // packets the timers looked at
};

// Random, non-zero stream ID. Identifies one lifetime of a link's stream, so a peer can tell a fresh stream (seq restarts at 1) from a stale one.
uint32_t makeStreamID(uint32_t different = 0);


class ReliableLink
{
public:
	using Clock							 = std::chrono::steady_clock;
	using TimePoint						 = Clock::time_point;

	// Ranges that fit into one Ack
	static constexpr size_t MaxAckRanges = (internal::MaxDatagramSize - BaseHeaderSize - AckFieldsSize) / SeqRangeSize;

	// budget: shared by every link that puts large messages together. None: every message is taken.
	explicit ReliableLink(const LinkTimings &timings = {}, uint32_t localStreamID = makeStreamID(), AssemblyBudget *budget = nullptr);

	// From now on the remote has to be heard from: it is asked for a sign of life after timings.keepAlive of silence,
	// and the link fails after timings.peerTimeout of it
	void						  supervise(TimePoint now);

	// --- Sending --------------------------------------------------------------

	// Asks the remote for a sign of life: it answers with an Ack
	void						  sendPing();

	// The next Ack or Ping, null if there is none. These are never held back.
	const OutgoingDatagram		 *peekAck();
	void						  commitAck();

	// The next datagram of that lane, null if the lane has nothing it may send right now. A message is taken from the
	// source when the previous one is on its way. The datagram only counts as sent with commit(), which has to follow
	// its peek() directly.
	const OutgoingDatagram		 *peek(Lane lane, TimePoint now, MessageSource &source);
	void						  commit(Lane lane, TimePoint now);

	// Everything above in one pass, for tests and benchmarks: Acks and Pings first, then the lanes in order of urgency
	std::vector<OutgoingDatagram> takeOutgoing(TimePoint now, MessageSource &source);

	// --- Receiving -------------------------------------------------------------

	void						  onPacket(const DecodedPacket &packet, TimePoint now);

	// The application does not keep up with what arrives: the remote is asked to hold Reliable and Bulk back
	void						  setPaused(bool paused);

	// --- Timers ----------------------------------------------------------------

	// Declares overdue packets lost, asks paused lanes again and detects a failed link
	void						  onTimer(TimePoint now);

	// When onTimer() has something to do next
	std::optional<TimePoint>	  nextDeadline() const;

	// --- Output ----------------------------------------------------------------

	std::vector<DeliveredMessage> takeDelivered() { return std::exchange(mDelivered, {}); }

	// --- State -----------------------------------------------------------------

	// The remote stopped answering: the link sends and takes nothing anymore
	bool						  hasFailed() const { return mFailed; }

	// A message of an acknowledged lane is on its way and not completely acknowledged yet
	bool						  hasPendingReliable() const;
	bool						  hasPending(Lane lane) const;

	// Media messages of the remote that were given up on because newer ones arrived
	uint64_t					  mediaIncomplete() const { return mMediaAssembler ? mMediaAssembler->abandoned() : 0; }

	// Data packets that were sent and are not acknowledged yet (those waiting for their retransmission included)
	size_t						  inFlightCount() const;
	size_t						  congestionWindow() const { return static_cast<size_t>(mCongestionWindow); }

	uint32_t					  localStreamID() const { return mLocalStreamID; }
	std::optional<uint32_t>		  remoteStreamID() const { return mRemoteStreamID; }
	const RttEstimator			 &rtt() const { return mRtt; }
	const LinkStats				 &stats() const { return mStats; }

private:
	static constexpr size_t StreamCount = 3; // Control, Reliable, Bulk

	struct InFlight
	{
		MessageBody message; // the fragment is message[offset, offset + length)
		uint32_t	offset{0};
		uint32_t	length{0};
		uint32_t	tag{0};
		uint32_t	totalLength{0};
		uint16_t	fragIndex{0};
		uint16_t	fragCount{1};
		uint16_t	transmissions{0};
		bool		lost{false};	 // waiting for its retransmission: not on the wire, no timer running
		TimePoint	sentAt{};		 // latest transmission
		uint64_t	transmission{0}; // ... and its position among everything this link sent
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
		size_t			count{1};
		size_t			next{0};
	};

	// One transmission, in the order they went out
	struct SentRecord
	{
		uint64_t transmission{0};
		uint64_t seq{0};
		Lane	 lane{Lane::Control};
	};

	// Everything one acknowledged lane needs, in both directions
	struct Stream
	{
		Stream(const size_t window, AssemblyBudget *budget) : inFlight(window), reorder(window), assembler(budget) {}

		// Send side
		std::optional<FragmentCursor> cursor;
		SequenceBuffer<InFlight>	  inFlight;
		std::deque<uint64_t>		  lost;				 // seqs waiting for their retransmission, the most urgent first
		uint64_t					  nextSendSeq{1};
		uint64_t					  sendBase{1};		 // lowest unacknowledged seq
		size_t						  onTheWire{0};		 // sent and neither acknowledged nor considered lost
		bool						  peerPaused{false}; // the remote asked for a pause
		std::optional<uint32_t>		  ackSerial;		 // of the Ack that pause state was taken from
		std::optional<TimePoint>	  probeAt;			 // paused: when the remote is asked again
		std::chrono::milliseconds	  probeInterval{0};

		// Receive side
		SequenceBuffer<BufferedData>  reorder;
		uint64_t					  nextExpected{1};
		bool						  budgetPaused{false}; // a message was refused for lack of room
		uint32_t					  refusedLength{0};
		MessageAssembler			  assembler;
	};

	static constexpr size_t	 windowOf(const Lane lane) { return lane == Lane::Control ? ControlWindow : lane == Lane::Reliable ? ReliableWindow : BulkWindow; }
	static constexpr uint8_t bitOf(const Lane lane) { return static_cast<uint8_t>(1u << std::to_underlying(lane)); }

	// Streams are created with their first use: most links never use every lane
	Stream					&streamFor(Lane lane);
	Stream					*existingStream(const Lane lane) { return mStreams[std::to_underlying(lane)].get(); }
	const Stream			*existingStream(const Lane lane) const { return mStreams[std::to_underlying(lane)].get(); }

	// Receiving
	void					 handleData(Lane lane, const PacketHeader &header, std::span<const uint8_t> body);
	void					 handleMedia(const PacketHeader &header, std::span<const uint8_t> body);
	void					 handleAck(Stream &stream, Lane lane, const PacketHeader &header, std::span<const uint8_t> body, TimePoint now);
	bool					 acceptInOrder(Stream &stream, Lane lane, const PacketHeader &header, std::span<const uint8_t> body);
	void					 drainReorderBuffer(Stream &stream, Lane lane);
	void					 retryRefused(Stream &stream, Lane lane);

	// Sending
	OutgoingDatagram		 makeAck(Lane lane);
	const OutgoingDatagram	*peekMedia(MessageSource &source);
	bool					 congestionWindowOpen(Lane lane) const;
	static InFlight			*nextLost(Stream &stream);
	static FragmentCursor	 cursorFor(OutboundMessage message);
	static InFlight			 fragmentAt(const FragmentCursor &cursor);
	PacketHeader			 headerOf(Lane lane, uint64_t seq, const InFlight &entry) const;
	const OutgoingDatagram	*offer(const PacketHeader &header, const InFlight &entry);
	void					 transmit(Stream &stream, Lane lane, uint64_t seq, InFlight &entry, TimePoint now);

	// Loss and congestion
	bool					 detectLosses();
	void					 markLost(const SentRecord &record, Stream &stream, InFlight &entry, bool timedOut);
	InFlight				*entryOf(const SentRecord &record);
	void					 setPeerPaused(Stream &stream, bool paused, TimePoint now);
	void					 updateProgressClock(TimePoint now, bool progress);
	void					 fail();

	OutgoingDatagram		 makeDatagram(const PacketHeader &header) const;
	PacketHeader			 makeHeader(PacketFlags flags, uint64_t seq) const;

	void					 resetStreams();


	LinkTimings				 mTimings;
	uint32_t				 mLocalStreamID;
	std::optional<uint32_t>	 mRemoteStreamID;
	RttEstimator			 mRtt;
	AssemblyBudget			*mBudget;

	std::array<std::unique_ptr<Stream>, StreamCount> mStreams;

	// Media: sent once, in fragments numbered by one counter
	std::optional<FragmentCursor>					 mMediaCursor;
	uint64_t										 mNextMediaSeq{1};
	std::unique_ptr<MediaAssembler>					 mMediaAssembler;
	uint32_t										 mMediaReceived{0};

	// Loss detection, for all lanes together: the oldest transmission that still counts is at the front
	std::deque<SentRecord>							 mSent;
	uint64_t										 mTransmissions{0};	 // counts every Data transmission
	uint64_t										 mLargestAcked{0};	 // the latest transmission that was acknowledged
	std::optional<TimePoint>						 mStalledSince;		 // data is unacknowledged: when the last acknowledgement arrived
	std::optional<TimePoint>						 mLastReceived;		 // supervised: when the remote was last heard from
	TimePoint										 mPingedAt{};		 // ... and when it was last asked
	int												 mTimeoutsInARow{0}; // retransmission timeouts since the last acknowledgement

	// Congestion control, for Reliable and Bulk together
	double											 mCongestionWindow;
	uint64_t										 mRecoveryStart{0}; // losses of transmissions up to this one already reduced the window

	bool											 mPaused{false};	// what the Acks of Reliable and Bulk tell the remote
	uint32_t										 mAckSerial{0};
	uint8_t											 mAckDue{0};		// lanes, as bits
	uint8_t											 mPingDue{0};		// lanes, as bits

	std::deque<OutgoingDatagram>					 mPendingAcks;		// built and not sent yet
	OutgoingDatagram								 mPeeked;			// what the last peek() returned

	std::vector<DeliveredMessage>					 mDelivered;
	bool											 mFailed{false};
	LinkStats										 mStats;
};

} // namespace netlink::channel
