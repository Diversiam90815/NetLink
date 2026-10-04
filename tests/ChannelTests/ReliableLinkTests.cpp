#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <optional>
#include <random>
#include <vector>

#include "Channel/Reliability/ReliableLink.h"
#include "QueueSource.h"

using namespace netlink;
using namespace netlink::channel;
using namespace std::chrono_literals;


namespace ChannelTests
{

// Two links wired back to back by a scripted wire, with a manual clock
class ReliableLinkTest : public ::testing::Test
{
protected:
	using Drop = std::function<bool(const PacketHeader &header)>; // true = lose the packet
	using Pass = std::vector<OutgoingDatagram>;

	// What an Ack says
	struct Ack
	{
		Lane	 lane{Lane::Control};
		uint64_t seq{0};
		bool	 paused{false};
		AckBody	 body;
	};

	static LinkTimings defaultTimings()
	{
		LinkTimings timings;
		timings.initialRto	= 100ms;
		timings.minRto		= 20ms;
		timings.maxRto		= 400ms;
		timings.peerTimeout = 2s;

		// These tests are about the protocol, not about congestion: the window never limits
		timings.fixedCwnd	= 4096;
		return timings;
	}

	// The fixture's timers with the real congestion window (32 packets, adapting)
	static LinkTimings congestionTimings()
	{
		LinkTimings timings = defaultTimings();
		timings.fixedCwnd	= 0;
		return timings;
	}

	LinkTimings					  timings = defaultTimings();
	ReliableLink				  a{timings, 0xAAAA0001};
	ReliableLink				  b{timings, 0xBBBB0001};
	FakeNet::QueueSource		  fromA; // what a has to send
	FakeNet::QueueSource		  fromB;
	ReliableLink::TimePoint		  now = ReliableLink::Clock::now();

	std::vector<DeliveredMessage> atA;
	std::vector<DeliveredMessage> atB;

	void						  recreate(const LinkTimings &newTimings)
	{
		timings = newTimings;
		a		= ReliableLink(timings, 0xAAAA0001);
		b		= ReliableLink(timings, 0xBBBB0001);
	}

	FakeNet::QueueSource &sourceOf(const ReliableLink &link) { return &link == &a ? fromA : fromB; }

	void				  send(ReliableLink &from, const Lane lane, const uint32_t tag, std::vector<uint8_t> body) { sourceOf(from).push(lane, tag, std::move(body)); }

	// One send pass of the link
	Pass				  take(ReliableLink &from) { return FakeNet::takeOutgoing(from, now, sourceOf(from)); }

	// Something acknowledged is still on its way, or did not even leave yet
	bool				  pending(const ReliableLink &link)
	{
		const auto &source = sourceOf(link);
		return link.hasPendingReliable() || source.waiting(Lane::Control) + source.waiting(Lane::Reliable) + source.waiting(Lane::Bulk) > 0;
	}

	void deliver(ReliableLink &to, const OutgoingDatagram &datagram)
	{
		const auto bytes  = datagram.bytes();
		const auto packet = decodePacket(bytes);
		ASSERT_TRUE(packet.has_value()) << "Links must only produce well formed packets";
		to.onPacket(*packet, now);
	}

	// Hands a whole send pass to the other link. Returns the number of datagrams that arrived.
	size_t deliverPass(ReliableLink &to, const Pass &pass, const Drop &drop = {})
	{
		size_t arrived = 0;

		for (const auto &datagram : pass)
		{
			const auto bytes  = datagram.bytes();
			const auto packet = decodePacket(bytes);
			EXPECT_TRUE(packet.has_value()) << "Links must only produce well formed packets";
			if (!packet || (drop && drop(packet->header)))
				continue;

			to.onPacket(*packet, now);
			++arrived;
		}

		collect();
		return arrived;
	}

	// Carries everything `from` produced so far to `to`. Returns the number of datagrams that arrived.
	size_t transfer(ReliableLink &from, ReliableLink &to, const Drop &drop = {}) { return deliverPass(to, take(from), drop); }

	void   collect()
	{
		for (auto &message : a.takeDelivered())
			atA.push_back(std::move(message));
		for (auto &message : b.takeDelivered())
			atB.push_back(std::move(message));
	}

	// Exchanges packets until nothing moves anymore
	void settle(const Drop &dropAtoB = {}, const Drop &dropBtoA = {})
	{
		for (int round = 0; round < 10'000; ++round)
		{
			if (transfer(a, b, dropAtoB) + transfer(b, a, dropBtoA) == 0)
				return;
		}
		FAIL() << "The links never settled";
	}

	void advance(std::chrono::microseconds step)
	{
		now += step;
		a.onTimer(now);
		b.onTimer(now);
	}

	static std::vector<uint8_t> bytes(std::initializer_list<uint8_t> values) { return values; }

	static std::vector<uint8_t> pattern(size_t size)
	{
		std::vector<uint8_t> body(size);
		for (size_t i = 0; i < size; ++i)
			body[i] = static_cast<uint8_t>(i * 13);
		return body;
	}

	static DecodedPacket decoded(const OutgoingDatagram &datagram, std::vector<uint8_t> &storage)
	{
		storage = datagram.bytes();
		return *decodePacket(storage);
	}

	// The datagrams of one lane and kind in a send pass
	static Pass only(const Pass &pass, const Lane lane, const PacketKind packetKind = PacketKind::Data)
	{
		Pass				 result;
		std::vector<uint8_t> storage;

		for (const auto &datagram : pass)
		{
			const auto packet = decoded(datagram, storage);
			if (packet.header.flags.kind() == packetKind && packet.header.flags.lane() == lane)
				result.push_back(datagram);
		}
		return result;
	}

	static size_t count(const Pass &pass, const PacketKind packetKind)
	{
		std::vector<uint8_t> storage;
		return static_cast<size_t>(std::ranges::count_if(pass, [&](const auto &datagram) { return decoded(datagram, storage).header.flags.kind() == packetKind; }));
	}

	// Seqs of the Data packets in one send pass, in order
	static std::vector<uint64_t> dataSeqs(const Pass &pass, std::optional<Lane> lane = {})
	{
		std::vector<uint64_t> seqs;
		std::vector<uint8_t>  storage;

		for (const auto &datagram : pass)
		{
			const auto packet = decoded(datagram, storage);
			if (packet.header.flags.kind() == PacketKind::Data && (!lane || packet.header.flags.lane() == *lane))
				seqs.push_back(packet.header.seq);
		}
		return seqs;
	}

	// The Acks in a send pass
	static std::vector<Ack> acks(const Pass &pass)
	{
		std::vector<Ack>	 result;
		std::vector<uint8_t> storage;

		for (const auto &datagram : pass)
		{
			const auto packet = decoded(datagram, storage);
			if (packet.header.flags.kind() != PacketKind::Ack)
				continue;

			result.push_back(
				{.lane = packet.header.flags.lane(), .seq = packet.header.seq, .paused = packet.header.flags.isPaused(), .body = decodeAck(packet.body).value_or(AckBody{})});
		}
		return result;
	}

	static Drop seqOf(const Lane lane, const uint64_t seq)
	{
		return [=](const PacketHeader &header) { return header.flags.kind() == PacketKind::Data && header.flags.lane() == lane && header.seq == seq; };
	}

	std::vector<uint32_t> tagsAtB() const
	{
		std::vector<uint32_t> tags;
		for (const auto &message : atB)
			tags.push_back(message.tag);
		return tags;
	}

	static bool failed(const ReliableLink &link) { return link.hasFailed(); }
};


// ---------------------------------------------------------------------------
// Data and Ack
// ---------------------------------------------------------------------------

TEST_F(ReliableLinkTest, HappyPath_DataAndAck)
{
	send(a, Lane::Reliable, 7, bytes({1, 2, 3}));
	EXPECT_TRUE(pending(a));

	EXPECT_EQ(transfer(a, b), 1u) << "One Data packet";
	EXPECT_EQ(a.inFlightCount(), 1u);
	ASSERT_EQ(atB.size(), 1u);
	EXPECT_EQ(atB[0].body, bytes({1, 2, 3}));
	EXPECT_EQ(atB[0].lane, Lane::Reliable);
	EXPECT_EQ(atB[0].tag, 7u);

	EXPECT_EQ(transfer(b, a), 1u) << "One Ack";
	EXPECT_EQ(a.inFlightCount(), 0u);
	EXPECT_FALSE(pending(a));

	EXPECT_TRUE(take(a).empty()) << "An Ack is not confirmed";
	EXPECT_EQ(a.stats().dataSent, 1u);
	EXPECT_EQ(b.stats().acksSent, 1u);

	// Nothing is left to retransmit on either side, and nothing to fail for
	advance(5s);
	EXPECT_TRUE(take(a).empty());
	EXPECT_TRUE(take(b).empty());
	EXPECT_EQ(a.stats().retransmissions, 0u);
	EXPECT_FALSE(failed(a));
}


TEST_F(ReliableLinkTest, OneAck_CoversAWholeBatch)
{
	for (uint8_t i = 0; i < 50; ++i)
		send(a, Lane::Reliable, 0, bytes({i}));

	EXPECT_EQ(transfer(a, b), 50u);
	ASSERT_EQ(atB.size(), 50u);

	const auto fromRemote = take(b);
	const auto answer	  = acks(fromRemote);

	ASSERT_EQ(fromRemote.size(), 1u) << "Everything that arrived since the last pass is acknowledged with one datagram";
	ASSERT_EQ(answer.size(), 1u);
	EXPECT_EQ(answer[0].lane, Lane::Reliable);
	EXPECT_EQ(answer[0].seq, 50u) << "The header names the highest seq received without a gap";
	EXPECT_TRUE(answer[0].body.ranges.empty());
	EXPECT_FALSE(answer[0].paused);

	deliverPass(a, fromRemote);
	EXPECT_FALSE(pending(a));
}


TEST_F(ReliableLinkTest, RequestResponse_CostsFourDatagrams)
{
	send(a, Lane::Reliable, 1, bytes({1}));

	const auto request = take(a);
	ASSERT_EQ(request.size(), 1u);
	deliverPass(b, request);

	send(b, Lane::Reliable, 2, bytes({2}));

	const auto response = take(b);
	EXPECT_EQ(response.size(), 2u) << "The Ack of the request and the response";
	deliverPass(a, response);

	const auto confirmation = take(a);
	EXPECT_EQ(confirmation.size(), 1u) << "The Ack of the response";
	deliverPass(b, confirmation);

	EXPECT_TRUE(take(a).empty());
	EXPECT_TRUE(take(b).empty());
	EXPECT_EQ(atA.size(), 1u);
	EXPECT_EQ(atB.size(), 1u);
}


TEST_F(ReliableLinkTest, LostData_IsRetransmittedAfterTheRto)
{
	send(a, Lane::Reliable, 0, bytes({7}));
	take(a); // lost on the wire

	advance(99ms);
	EXPECT_TRUE(take(a).empty()) << "Not before the RTO";

	advance(1ms);
	settle();

	ASSERT_EQ(atB.size(), 1u);
	EXPECT_EQ(atB[0].body, bytes({7}));
	EXPECT_EQ(a.stats().retransmissions, 1u);
	EXPECT_FALSE(pending(a));
}


TEST_F(ReliableLinkTest, LostAck_DataIsResentButDeliveredOnce)
{
	send(a, Lane::Reliable, 0, bytes({7}));
	transfer(a, b);
	take(b); // the Ack is lost
	ASSERT_EQ(atB.size(), 1u);

	advance(100ms);
	settle();

	EXPECT_EQ(atB.size(), 1u) << "A retransmitted Data packet must not be delivered twice";
	EXPECT_GE(b.stats().duplicatesReceived, 1u);
	EXPECT_FALSE(pending(a)) << "The duplicate is acknowledged again";
}


TEST_F(ReliableLinkTest, LostAck_IsCoveredByTheNextOne)
{
	send(a, Lane::Reliable, 0, bytes({1}));
	transfer(a, b);
	take(b); // the Ack for seq 1 is lost

	send(a, Lane::Reliable, 0, bytes({2}));
	transfer(a, b);

	const auto fromRemote = take(b);
	const auto answer	  = acks(fromRemote);
	ASSERT_EQ(answer.size(), 1u);
	EXPECT_EQ(answer[0].seq, 2u) << "It says that everything up to seq 2 arrived";

	deliverPass(a, fromRemote);

	EXPECT_FALSE(pending(a)) << "Seq 1 is acknowledged without an Ack of its own";
	EXPECT_EQ(a.stats().retransmissions, 0u) << "... and without sending it again";
}


TEST_F(ReliableLinkTest, ReorderedData_IsDeliveredInOrder)
{
	send(a, Lane::Reliable, 0, bytes({1}));
	send(a, Lane::Reliable, 0, bytes({2}));
	send(a, Lane::Reliable, 0, bytes({3}));

	const auto datagrams = take(a);
	ASSERT_EQ(datagrams.size(), 3u);

	deliver(b, datagrams[2]);
	collect();
	EXPECT_TRUE(atB.empty()) << "Seq 3 waits for the gap before it to close";

	const auto early = acks(take(b));
	ASSERT_EQ(early.size(), 1u);
	EXPECT_EQ(early[0].seq, 0u) << "Nothing arrived in order yet";
	EXPECT_EQ(early[0].body.ranges, (std::vector<SeqRange>{{3, 1}})) << "A buffered packet is acknowledged right away";

	deliver(b, datagrams[0]);
	deliver(b, datagrams[1]);
	collect();

	ASSERT_EQ(atB.size(), 3u);
	EXPECT_EQ(atB[0].body, bytes({1}));
	EXPECT_EQ(atB[1].body, bytes({2}));
	EXPECT_EQ(atB[2].body, bytes({3}));
}


TEST_F(ReliableLinkTest, Ack_ListsOnlyTheReorderBuffer)
{
	for (uint8_t i = 1; i <= 10; ++i)
		send(a, Lane::Reliable, 0, bytes({i}));

	const auto datagrams = take(a);
	ASSERT_EQ(datagrams.size(), 10u);

	for (const size_t arrived : {0u, 1u, 2u, 3u, 6u, 7u, 9u})
		deliver(b, datagrams[arrived]);

	auto answer = acks(take(b));
	ASSERT_EQ(answer.size(), 1u);
	EXPECT_EQ(answer[0].seq, 4u);
	EXPECT_EQ(answer[0].body.ranges, (std::vector<SeqRange>{{7, 2}, {10, 1}})) << "What arrived in order is named by the seq alone";

	deliver(b, datagrams[4]);
	deliver(b, datagrams[5]);

	answer = acks(take(b));
	ASSERT_EQ(answer.size(), 1u);
	EXPECT_EQ(answer[0].seq, 8u);
	EXPECT_EQ(answer[0].body.ranges, (std::vector<SeqRange>{{10, 1}})) << "What left the reorder buffer is not listed anymore";

	deliver(b, datagrams[8]);

	answer = acks(take(b));
	ASSERT_EQ(answer.size(), 1u);
	EXPECT_EQ(answer[0].seq, 10u);
	EXPECT_TRUE(answer[0].body.ranges.empty());
}


TEST_F(ReliableLinkTest, DuplicatedDatagram_IsDeliveredOnce)
{
	send(a, Lane::Reliable, 0, bytes({1}));
	const auto datagrams = take(a);

	deliver(b, datagrams[0]);
	take(b);

	deliver(b, datagrams[0]);
	collect();

	EXPECT_EQ(atB.size(), 1u);
	EXPECT_EQ(b.stats().duplicatesReceived, 1u);

	const auto again = acks(take(b));
	ASSERT_EQ(again.size(), 1u) << "The duplicate is acknowledged too, its sender may have missed the first Ack";
	EXPECT_EQ(again[0].seq, 1u);
}


TEST_F(ReliableLinkTest, BrokenAcknowledgements_AreIgnored)
{
	send(a, Lane::Reliable, 0, bytes({1}));
	transfer(a, b);
	take(b);

	PacketHeader header;
	header.flags			 = PacketFlags::ack(Lane::Reliable);
	header.srcStreamID = b.localStreamID();
	header.dstStreamID = a.localStreamID();
	header.seq		   = 1;

	const auto withoutFields = encodePacket(header, bytes({1}));
	a.onPacket(*decodePacket(withoutFields), now);
	EXPECT_EQ(a.inFlightCount(), 1u) << "An Ack without its two fields";

	const auto halfARange = encodePacket(header, bytes({0, 0, 0, 1, 0, 0, 0, 0, 1, 2, 3}));
	a.onPacket(*decodePacket(halfARange), now);
	EXPECT_EQ(a.inFlightCount(), 1u) << "An Ack whose ranges are cut off";

	header.flags		 = PacketFlags::ack(Lane::Bulk);
	const auto otherLane = encodePacket(header, encodeAck({.serial = 1, .mediaReceived = 0, .ranges = {}}, 1));
	a.onPacket(*decodePacket(otherLane), now);
	EXPECT_EQ(a.inFlightCount(), 1u) << "An Ack for a lane nothing was sent on";
}


TEST_F(ReliableLinkTest, AcknowledgementOfUnsentSeqs_ReleasesOnlyWhatWasSent)
{
	send(a, Lane::Reliable, 0, bytes({1}));
	take(a);

	PacketHeader header;
	header.flags		= PacketFlags::ack(Lane::Reliable);
	header.srcStreamID = b.localStreamID();
	header.seq		   = 1'000'000; // far beyond anything a sent

	const auto datagram = encodePacket(header, encodeAck({.serial = 1, .mediaReceived = 0, .ranges = {{5'000'000, 60'000}}}, 10));
	a.onPacket(*decodePacket(datagram), now);

	EXPECT_FALSE(pending(a)) << "Seq 1 is covered";

	// The stream continues with seq 2 as if nothing happened
	send(a, Lane::Reliable, 0, bytes({2}));
	EXPECT_EQ(dataSeqs(take(a)), (std::vector<uint64_t>{2}));
}


// ---------------------------------------------------------------------------
// Liveness
// ---------------------------------------------------------------------------

TEST_F(ReliableLinkTest, Unsupervised_NeverAsksAndNeverGivesUp)
{
	send(a, Lane::Reliable, 0, bytes({1}));
	settle();

	advance(10 * timings.peerTimeout);

	EXPECT_TRUE(take(a).empty());
	EXPECT_FALSE(failed(a));
	EXPECT_FALSE(a.nextDeadline().has_value());
}


TEST_F(ReliableLinkTest, Supervised_AsksARemoteThatWentQuiet)
{
	send(a, Lane::Reliable, 0, bytes({1}));
	settle();
	a.supervise(now);

	ASSERT_EQ(a.nextDeadline(), now + timings.keepAlive);

	advance(timings.keepAlive - 1ms);
	EXPECT_TRUE(take(a).empty()) << "Not before it was quiet for that long";

	advance(1ms);
	const auto asked = take(a);
	ASSERT_EQ(count(asked, PacketKind::Ping), 1u);

	deliverPass(b, asked);
	const auto answer = take(b);
	ASSERT_EQ(acks(answer).size(), 1u);
	deliverPass(a, answer);

	EXPECT_EQ(a.nextDeadline(), now + timings.keepAlive) << "The answer counts: the remote is asked again that much later";
	EXPECT_FALSE(failed(a));
}


TEST_F(ReliableLinkTest, Supervised_AsksOncePerInterval)
{
	timings.peerTimeout = 10 * timings.keepAlive;
	recreate(timings);

	send(a, Lane::Reliable, 0, bytes({1}));
	settle();
	a.supervise(now);

	size_t pings = 0;
	for (int i = 0; i < 30; ++i)
	{
		advance(timings.keepAlive / 10);
		pings += count(take(a), PacketKind::Ping);
	}

	EXPECT_EQ(pings, 3u) << "Three intervals of silence, three questions";
}


TEST_F(ReliableLinkTest, Supervised_FailsWhenTheRemoteStaysSilent)
{
	send(a, Lane::Reliable, 0, bytes({1}));
	settle();
	a.supervise(now);

	const auto started = now;
	while (!failed(a) && now - started < 10 * timings.peerTimeout)
	{
		take(a); // the questions get lost
		advance(50ms);
	}

	ASSERT_TRUE(failed(a));
	EXPECT_GE(now - started, timings.peerTimeout);
	EXPECT_LE(now - started, timings.peerTimeout + 100ms);
}


TEST_F(ReliableLinkTest, Supervised_AnyPacketCountsAsASignOfLife)
{
	send(a, Lane::Reliable, 0, bytes({1}));
	settle();
	a.supervise(now);

	// B keeps sending, A never has to ask
	for (int i = 0; i < 40; ++i)
	{
		advance(timings.keepAlive / 2);
		send(b, Lane::Media, 0, bytes({1}));
		const auto fromA = take(a);
		EXPECT_EQ(count(fromA, PacketKind::Ping), 0u);
		deliverPass(a, take(b));
	}

	EXPECT_FALSE(failed(a));
}


TEST_F(ReliableLinkTest, Supervised_DoesNotAskARemoteItDoesNotKnowYet)
{
	a.supervise(now);
	send(a, Lane::Control, 1, bytes({1}));
	take(a); // lost

	advance(timings.keepAlive);

	EXPECT_EQ(count(take(a), PacketKind::Ping), 0u) << "Only the first Control packet may go out without the remote's stream ID";
	EXPECT_EQ(a.stats().pingsSent, 0u);
}


// ---------------------------------------------------------------------------
// Failure and restart
// ---------------------------------------------------------------------------

TEST_F(ReliableLinkTest, UnacknowledgedData_FailsTheLinkAfterThePeerTimeout)
{
	const auto started = now;

	send(a, Lane::Reliable, 0, bytes({1}));

	bool linkFailed = false;
	for (int i = 0; i < 100 && !linkFailed; ++i)
	{
		take(a); // everything is lost
		advance(100ms);
		linkFailed = failed(a);
	}

	ASSERT_TRUE(linkFailed);
	EXPECT_GE(now - started, timings.peerTimeout) << "Not before the timeout";
	EXPECT_LE(now - started, timings.peerTimeout + 200ms);
	EXPECT_GT(a.stats().retransmissions, 0u) << "It kept trying until then";
	EXPECT_FALSE(a.hasPendingReliable()) << "The failed stream is discarded";

	send(a, Lane::Reliable, 0, bytes({2}));
	EXPECT_TRUE(take(a).empty()) << "A failed link sends nothing anymore: its session is over";
	EXPECT_FALSE(a.nextDeadline().has_value());
}


TEST_F(ReliableLinkTest, SlowProgress_DoesNotFailTheLink)
{
	for (uint8_t i = 0; i < 40; ++i)
		send(a, Lane::Reliable, 0, bytes({i}));

	// A path that is nearly dead: one Data packet gets through per timeout. That is slow, but it is not a failed link.
	for (int i = 0; i < 200 && pending(a); ++i)
	{
		bool	   passed	 = false;
		const Drop allButOne = [&passed](const PacketHeader &header)
		{
			if (header.flags.kind() != PacketKind::Data)
				return false;

			return std::exchange(passed, true);
		};

		transfer(a, b, allButOne);
		transfer(b, a);
		advance(400ms);

		ASSERT_FALSE(failed(a)) << "after " << i << " rounds";
	}

	EXPECT_FALSE(pending(a));
	EXPECT_EQ(atB.size(), 40u) << "Everything arrives, long after the peer timeout would have passed";
}


TEST_F(ReliableLinkTest, PacketOfAnotherRemoteStream_IsDropped)
{
	send(a, Lane::Reliable, 0, bytes({1}));
	settle();
	ASSERT_EQ(a.remoteStreamID(), b.localStreamID());

	// B was started again: a brand-new link with a new stream ID. That is another session, not this one.
	ReliableLink		 restarted(timings, 0xBBBB0002);
	FakeNet::QueueSource fromRestarted;

	fromRestarted.push(Lane::Control, 0, bytes({9}));
	deliverPass(a, FakeNet::takeOutgoing(restarted, now, fromRestarted));

	EXPECT_EQ(a.remoteStreamID(), b.localStreamID()) << "A link talks to one stream for as long as it lives";
	EXPECT_TRUE(atA.empty());
	EXPECT_EQ(a.stats().staleDropped, 1u);
	EXPECT_TRUE(take(a).empty()) << "... and does not acknowledge what is not meant for it";
}


TEST_F(ReliableLinkTest, PacketForAnOldStream_IsDropped)
{
	send(a, Lane::Reliable, 0, bytes({1}));
	settle();

	PacketHeader header;
	header.flags		= PacketFlags::data(Lane::Reliable);
	header.srcStreamID = a.localStreamID();
	header.dstStreamID = 0x12345678; // not B
	header.seq		   = 2;

	const auto datagram = encodePacket(header, bytes({5}));
	b.onPacket(*decodePacket(datagram), now);
	collect();

	EXPECT_EQ(b.stats().staleDropped, 1u);
	EXPECT_EQ(atB.size(), 1u);
	EXPECT_TRUE(take(b).empty()) << "A stale packet is not acknowledged";
}


// ---------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------

TEST_F(ReliableLinkTest, DataBeyondTheReceiveWindow_IsNotAcknowledged)
{
	PacketHeader header;
	header.flags		= PacketFlags::data(Lane::Reliable);
	header.srcStreamID = a.localStreamID();
	header.seq			= 1 + ReliableWindow;

	const auto datagram = encodePacket(header, bytes({5}));
	b.onPacket(*decodePacket(datagram), now);

	EXPECT_EQ(b.stats().outOfWindowDropped, 1u);
	EXPECT_TRUE(take(b).empty());
}


TEST_F(ReliableLinkTest, SendWindowLimitsPacketsInFlight)
{
	const size_t total = ReliableWindow + 44;
	for (size_t i = 0; i < total; ++i)
		send(a, Lane::Reliable, 0, {static_cast<uint8_t>(i), static_cast<uint8_t>(i >> 8)});

	const auto first = take(a);
	EXPECT_EQ(first.size(), ReliableWindow) << "Never more unacknowledged seqs than the receiver can buffer";
	EXPECT_EQ(a.inFlightCount(), ReliableWindow);
	EXPECT_EQ(fromA.waiting(Lane::Reliable), 44u) << "What cannot go out yet is not even taken";
	EXPECT_TRUE(take(a).empty()) << "The window is full until Acks arrive";

	deliverPass(b, first);
	settle();

	ASSERT_EQ(atB.size(), total);
	for (size_t i = 0; i < total; ++i)
		EXPECT_EQ(atB[i].body, (std::vector<uint8_t>{static_cast<uint8_t>(i), static_cast<uint8_t>(i >> 8)})) << "in order at " << i;
	EXPECT_FALSE(pending(a));
}


TEST_F(ReliableLinkTest, ControlLane_HasASmallWindow)
{
	for (uint8_t i = 0; i < 40; ++i)
		send(a, Lane::Control, 0, bytes({i}));

	EXPECT_EQ(take(a).size(), ControlWindow);

	settle();
	advance(100ms);
	settle();
	EXPECT_EQ(atB.size(), 40u);
}


// ---------------------------------------------------------------------------
// Lanes
// ---------------------------------------------------------------------------

TEST_F(ReliableLinkTest, Lanes_AreOrderedIndependently)
{
	send(a, Lane::Reliable, 1, bytes({1}));
	send(a, Lane::Reliable, 2, bytes({2}));
	send(a, Lane::Bulk, 3, bytes({3}));
	send(a, Lane::Bulk, 4, bytes({4}));
	send(a, Lane::Control, 5, bytes({5}));

	// The first Reliable packet is lost
	transfer(a, b, seqOf(Lane::Reliable, 1));

	EXPECT_EQ(tagsAtB(), (std::vector<uint32_t>{5, 3, 4})) << "Only the lane with the gap waits";

	settle();

	EXPECT_EQ(tagsAtB(), (std::vector<uint32_t>{5, 3, 4, 1, 2})) << "... and delivers in its own order once the gap is closed";
}


TEST_F(ReliableLinkTest, BulkLoss_DoesNotDelayReliable)
{
	send(a, Lane::Bulk, 1, pattern(10'000));
	send(a, Lane::Reliable, 2, bytes({7}));

	transfer(a, b, seqOf(Lane::Bulk, 3));

	ASSERT_EQ(atB.size(), 1u) << "The small message does not wait for the blob in front of it";
	EXPECT_EQ(atB[0].tag, 2u);

	settle();

	ASSERT_EQ(atB.size(), 2u);
	EXPECT_EQ(atB[1].body, pattern(10'000));
	EXPECT_EQ(atB[1].lane, Lane::Bulk);
}


TEST_F(ReliableLinkTest, ControlSignal_IsNotHeldUpByAFullCongestionWindow)
{
	recreate(congestionTimings());

	for (int i = 0; i < 100; ++i)
		send(a, Lane::Reliable, 0, bytes({1}));

	EXPECT_EQ(dataSeqs(take(a)).size(), InitialCongestionWindow) << "The congestion window is full, and nothing of it is acknowledged";

	send(a, Lane::Control, 0, bytes({42}));

	const auto pass = take(a);
	ASSERT_EQ(pass.size(), 1u) << "The congestion window only holds Reliable and Bulk back";
	EXPECT_EQ(dataSeqs(pass, Lane::Control), (std::vector<uint64_t>{1}));

	deliverPass(b, pass);

	ASSERT_EQ(atB.size(), 1u);
	EXPECT_EQ(atB[0].lane, Lane::Control);
	EXPECT_EQ(atB[0].body, bytes({42}));
}


TEST_F(ReliableLinkTest, ReliableAndBulk_ShareTheCongestionWindow)
{
	recreate(congestionTimings());

	for (int i = 0; i < 20; ++i)
		send(a, Lane::Reliable, 0, bytes({1}));
	for (int i = 0; i < 100; ++i)
		send(a, Lane::Bulk, 0, bytes({2}));

	const auto pass = take(a);

	EXPECT_EQ(dataSeqs(pass, Lane::Reliable).size(), 20u);
	EXPECT_EQ(dataSeqs(pass, Lane::Bulk).size(), InitialCongestionWindow - 20) << "Bulk gets what Reliable leaves of the window";
}


TEST_F(ReliableLinkTest, SendPass_GoesInOrderOfUrgency)
{
	// Something to acknowledge
	send(b, Lane::Reliable, 0, bytes({9}));
	transfer(b, a);

	send(a, Lane::Bulk, 0, bytes({1}));
	send(a, Lane::Reliable, 0, bytes({2}));
	send(a, Lane::Media, 0, bytes({3}));
	send(a, Lane::Control, 0, bytes({4}));

	const auto pass = take(a);
	ASSERT_EQ(pass.size(), 5u);

	std::vector<DecodedPacket>		  packets;
	std::vector<std::vector<uint8_t>> storage(pass.size());
	for (size_t i = 0; i < pass.size(); ++i)
		packets.push_back(decoded(pass[i], storage[i]));

	EXPECT_EQ(packets[0].header.flags.kind(), PacketKind::Ack) << "The remote's sending depends on Acks";
	EXPECT_EQ(packets[1].header.flags.lane(), Lane::Control);
	EXPECT_EQ(packets[2].header.flags.lane(), Lane::Media);
	EXPECT_EQ(packets[3].header.flags.lane(), Lane::Reliable);
	EXPECT_EQ(packets[4].header.flags.lane(), Lane::Bulk);
}


TEST_F(ReliableLinkTest, Lanes_AreAcknowledgedSeparately)
{
	send(a, Lane::Control, 0, bytes({1}));
	send(a, Lane::Reliable, 0, bytes({2}));
	send(a, Lane::Reliable, 0, bytes({3}));
	send(a, Lane::Bulk, 0, bytes({4}));
	transfer(a, b);

	const auto answer = acks(take(b));
	ASSERT_EQ(answer.size(), 3u);

	EXPECT_EQ(answer[0].lane, Lane::Control);
	EXPECT_EQ(answer[0].seq, 1u);
	EXPECT_EQ(answer[1].lane, Lane::Reliable);
	EXPECT_EQ(answer[1].seq, 2u);
	EXPECT_EQ(answer[2].lane, Lane::Bulk);
	EXPECT_EQ(answer[2].seq, 1u);
	EXPECT_LT(answer[0].body.serial, answer[1].body.serial) << "Every Ack of a link has a serial of its own";
	EXPECT_LT(answer[1].body.serial, answer[2].body.serial);
}


// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

TEST_F(ReliableLinkTest, LargeMessage_ArrivesAsOneMessageWithItsTag)
{
	const auto body = pattern(100 * 1024);
	send(a, Lane::Reliable, 0xCAFE, body);

	settle();

	ASSERT_EQ(atB.size(), 1u) << "Fragments are put together by the link";
	EXPECT_EQ(atB[0].body, body);
	EXPECT_EQ(atB[0].tag, 0xCAFEu);
	EXPECT_EQ(b.stats().delivered, 1u);
	EXPECT_GT(a.stats().dataSent, 80u);
}


TEST_F(ReliableLinkTest, Fragments_AreSentFromTheMessageWithoutCopyingIt)
{
	send(a, Lane::Reliable, 0, pattern(5000));

	const auto pass = take(a);
	ASSERT_EQ(pass.size(), 5u);

	for (size_t i = 0; i < pass.size(); ++i)
	{
		ASSERT_TRUE(pass[i].message) << "A fragment refers to its message";
		EXPECT_EQ(pass[i].message, pass[0].message) << "... the same one for every fragment";
		EXPECT_EQ(pass[i].offset, i * MaxFragmentBody);
		EXPECT_TRUE(pass[i].owned.empty());
	}
}


TEST_F(ReliableLinkTest, LargeMessage_IsReassembledUnderLoss)
{
	const auto body = pattern(100 * 1024);
	send(a, Lane::Reliable, 3, body);

	std::mt19937 random(3);
	auto		 lossy = [&random](const PacketHeader &) { return std::uniform_real_distribution<double>(0.0, 1.0)(random) < 0.2; };

	for (int i = 0; i < 1000 && pending(a); ++i)
	{
		transfer(a, b, lossy);
		transfer(b, a, lossy);
		advance(50ms);
	}
	settle();

	ASSERT_FALSE(pending(a));
	EXPECT_FALSE(failed(a));

	ASSERT_EQ(atB.size(), 1u);
	EXPECT_EQ(atB[0].body, body);
	EXPECT_GT(a.stats().retransmissions, 0u) << "The loss must actually have been exercised";
}


TEST_F(ReliableLinkTest, ManyMessagesUnderLossDuplicationAndReordering)
{
	std::mt19937 random(11);
	auto		 roll	= [&random](double p) { return std::uniform_real_distribution<double>(0.0, 1.0)(random) < p; };

	const int	 total	= 2000;
	int			 queued = 0;

	for (int step = 0; step < 20'000 && (queued < total || pending(a)); ++step)
	{
		while (queued < total && fromA.waiting(Lane::Reliable) < 64)
		{
			send(a, Lane::Reliable, static_cast<uint32_t>(queued), {static_cast<uint8_t>(queued), static_cast<uint8_t>(queued >> 8)});
			++queued;
		}

		// A misbehaving network in both directions
		for (auto *pair : {&a, &b})
		{
			ReliableLink &from		= *pair;
			ReliableLink &to		= pair == &a ? b : a;
			auto		  datagrams = take(from);
			std::shuffle(datagrams.begin(), datagrams.end(), random);

			for (const auto &datagram : datagrams)
			{
				if (roll(0.25))
					continue;
				deliver(to, datagram);
				if (roll(0.1))
					deliver(to, datagram);
			}
		}

		collect();
		advance(10ms);
	}

	EXPECT_FALSE(failed(a));
	ASSERT_EQ(atB.size(), static_cast<size_t>(total)) << "Every message exactly once";
	for (int i = 0; i < total; ++i)
	{
		ASSERT_EQ(atB[i].body, (std::vector<uint8_t>{static_cast<uint8_t>(i), static_cast<uint8_t>(i >> 8)})) << "in order at " << i;
		ASSERT_EQ(atB[i].tag, static_cast<uint32_t>(i));
	}
}


TEST_F(ReliableLinkTest, LargeMessage_DeliveredUnderLossWithCongestionControl)
{
	recreate(congestionTimings());

	const auto body = pattern(512 * 1024);
	send(a, Lane::Bulk, 0, body);

	std::mt19937 random(5);
	auto		 lossy = [&random](const PacketHeader &) { return std::uniform_real_distribution<double>(0.0, 1.0)(random) < 0.2; };

	for (int i = 0; i < 50'000 && pending(a); ++i)
	{
		transfer(a, b, lossy);
		transfer(b, a, lossy);
		advance(5ms);
	}
	settle();

	ASSERT_FALSE(pending(a));
	EXPECT_FALSE(failed(a)) << "A lossy path is slow, not broken";

	ASSERT_EQ(atB.size(), 1u);
	EXPECT_EQ(atB[0].body, body);
	EXPECT_GT(a.stats().fastRetransmissions, 0u) << "Losses must have been repaired without waiting for the timeout as well";
}


TEST_F(ReliableLinkTest, NoDatagramExceedsTheMaximumSize)
{
	send(a, Lane::Reliable, 0, std::vector<uint8_t>(10'000, 1));

	const auto pass = take(a);
	ASSERT_FALSE(pass.empty());

	for (const auto &datagram : pass)
		EXPECT_LE(datagram.bytes().size(), internal::MaxDatagramSize);

	EXPECT_EQ(pass.front().bytes().size(), internal::MaxDatagramSize) << "The first fragment, which carries every extension, uses the datagram completely";
}


TEST_F(ReliableLinkTest, RttIsSampledFromUnretransmittedPackets)
{
	send(a, Lane::Reliable, 0, bytes({1}));
	transfer(a, b);
	now += 30ms;
	transfer(b, a);

	ASSERT_TRUE(a.rtt().hasSample());
	EXPECT_EQ(a.rtt().srtt(), 30ms);
}


// ---------------------------------------------------------------------------
// Media
// ---------------------------------------------------------------------------

TEST_F(ReliableLinkTest, Media_IsNeitherAckedNorRetransmitted)
{
	send(a, Lane::Media, 11, bytes({1}));
	send(a, Lane::Media, 12, bytes({2}));
	send(a, Lane::Media, 13, bytes({3}));

	const auto datagrams = take(a);
	ASSERT_EQ(datagrams.size(), 3u);

	deliver(b, datagrams[1]);
	deliver(b, datagrams[0]); // overtaken; the third one is lost
	collect();

	EXPECT_EQ(tagsAtB(), (std::vector<uint32_t>{12, 11})) << "Delivered as it arrives";
	EXPECT_EQ(atB[0].lane, Lane::Media);
	EXPECT_TRUE(take(b).empty()) << "Media is never acknowledged";
	EXPECT_FALSE(a.hasPendingReliable());

	advance(5s);
	EXPECT_TRUE(take(a).empty()) << "... and never sent again";
	EXPECT_FALSE(failed(a));
	EXPECT_EQ(a.stats().mediaSent, 3u);
	EXPECT_EQ(a.stats().retransmissions, 0u);
}


TEST_F(ReliableLinkTest, Media_LargeMessageIsPutTogetherInAnyOrder)
{
	const auto body = pattern(50'000);
	send(a, Lane::Media, 4, body);

	auto datagrams = take(a);
	ASSERT_EQ(datagrams.size(), fragmentsOf(body.size()));

	std::mt19937 random(9);
	std::ranges::shuffle(datagrams, random);
	deliverPass(b, datagrams);

	ASSERT_EQ(atB.size(), 1u);
	EXPECT_EQ(atB[0].body, body);
	EXPECT_EQ(atB[0].tag, 4u);
}


TEST_F(ReliableLinkTest, Media_DoesNotWaitForTheCongestionWindow)
{
	recreate(congestionTimings());

	for (int i = 0; i < 100; ++i)
		send(a, Lane::Reliable, 0, bytes({1}));
	EXPECT_EQ(take(a).size(), InitialCongestionWindow);

	send(a, Lane::Media, 0, bytes({2}));

	const auto pass = take(a);
	ASSERT_EQ(pass.size(), 1u);
	EXPECT_EQ(dataSeqs(pass, Lane::Media).size(), 1u);
}


TEST_F(ReliableLinkTest, MediaReceived_IsReportedWithEveryAck)
{
	for (uint8_t i = 0; i < 5; ++i)
		send(a, Lane::Media, 0, bytes({i}));
	send(a, Lane::Reliable, 0, bytes({9}));

	transfer(a, b, seqOf(Lane::Media, 2));
	transfer(b, a);

	EXPECT_EQ(a.stats().mediaSent, 5u);
	EXPECT_EQ(a.stats().mediaReceivedByPeer, 4u) << "The sender learns how much of its media arrives";
}


// ---------------------------------------------------------------------------
// Peek and commit
// ---------------------------------------------------------------------------

TEST_F(ReliableLinkTest, Peek_IsIdempotent)
{
	send(b, Lane::Reliable, 0, bytes({9}));
	transfer(b, a); // something to acknowledge

	send(a, Lane::Reliable, 7, pattern(3000));
	send(a, Lane::Media, 8, bytes({1}));

	for (const Lane lane : {Lane::Reliable, Lane::Media})
	{
		const auto *first = a.peek(lane, now, fromA);
		ASSERT_NE(first, nullptr);
		const auto	offered = first->bytes();

		const auto *again	= a.peek(lane, now, fromA);
		ASSERT_NE(again, nullptr);
		EXPECT_EQ(again->bytes(), offered) << "Until it is committed, the same datagram is offered";
	}

	const auto *ack = a.peekAck();
	ASSERT_NE(ack, nullptr);
	const auto offeredAck = ack->bytes();
	ASSERT_NE(a.peekAck(), nullptr);
	EXPECT_EQ(a.peekAck()->bytes(), offeredAck);

	EXPECT_EQ(a.peek(Lane::Control, now, fromA), nullptr) << "Nothing was queued there";

	EXPECT_EQ(a.inFlightCount(), 0u) << "Nothing counts as sent";
	EXPECT_EQ(a.stats().dataSent, 0u);
	EXPECT_EQ(a.stats().mediaSent, 0u);
	EXPECT_EQ(a.nextDeadline(), std::nullopt) << "No timer runs for a datagram that did not go out";

	settle();
	EXPECT_EQ(atB.size(), 2u) << "Everything that was only looked at still arrives";
	EXPECT_EQ(a.stats().retransmissions, 0u);
}


TEST_F(ReliableLinkTest, AbortedDatagram_LeavesNoTrace)
{
	const auto body = pattern(3000); // three fragments
	send(a, Lane::Reliable, 0, body);

	const auto *first = a.peek(Lane::Reliable, now, fromA);
	ASSERT_NE(first, nullptr);
	const OutgoingDatagram sent = *first;
	a.commit(Lane::Reliable, now);

	// The socket refuses the second fragment: it is not committed
	const auto *second = a.peek(Lane::Reliable, now, fromA);
	ASSERT_NE(second, nullptr);
	const auto refused = second->bytes();

	EXPECT_EQ(a.inFlightCount(), 1u);
	EXPECT_EQ(a.stats().dataSent, 1u);

	now += 1ms;
	const auto pass = take(a);

	ASSERT_EQ(pass.size(), 2u);
	EXPECT_EQ(pass[0].bytes(), refused) << "The refused datagram is offered again, with the same seq";
	EXPECT_EQ(dataSeqs(pass), (std::vector<uint64_t>{2, 3}));

	deliver(b, sent);
	deliverPass(b, pass);
	settle();

	ASSERT_EQ(atB.size(), 1u);
	EXPECT_EQ(atB[0].body, body);
	EXPECT_EQ(a.stats().dataSent, 3u);
	EXPECT_EQ(a.stats().retransmissions, 0u) << "A datagram that never went out was not lost either";
}


// ---------------------------------------------------------------------------
// Timers
// ---------------------------------------------------------------------------

TEST_F(ReliableLinkTest, NextDeadline_IsExact)
{
	EXPECT_EQ(a.nextDeadline(), std::nullopt);

	send(a, Lane::Reliable, 0, bytes({1}));
	const auto pass = take(a);

	EXPECT_EQ(a.nextDeadline(), now + 100ms) << "The retransmission timeout of the only packet";

	now += 30ms;
	deliverPass(b, pass);
	transfer(b, a);

	EXPECT_EQ(a.nextDeadline(), std::nullopt) << "Acknowledged: there is nothing to wake up for";
	EXPECT_EQ(b.nextDeadline(), std::nullopt) << "An Ack is not waited for";
}


TEST_F(ReliableLinkTest, TimerWork_IsProportionalToExpiredPackets)
{
	const auto started = now;

	// 200 packets on the wire, sent 100 us apart, and none of them is acknowledged
	for (uint32_t i = 0; i < 200; ++i)
	{
		send(a, Lane::Reliable, i, bytes({1}));
		ASSERT_EQ(take(a).size(), 1u);
		now += 100us;
	}

	const auto before = a.stats().timerWork;

	now				  = started + 100ms;
	a.onTimer(now);

	EXPECT_LE(a.stats().timerWork - before, 2u) << "One packet is overdue: the timer looks at it and at the one behind it, not at all 200";
	EXPECT_EQ(dataSeqs(take(a)), (std::vector<uint64_t>{1})) << "Only the oldest packet timed out";
	EXPECT_EQ(a.nextDeadline(), started + 100us + 200ms) << "The next one is due when its own timeout ends, which doubled";
}


// ---------------------------------------------------------------------------
// Congestion window
// ---------------------------------------------------------------------------

class CongestionTest : public ReliableLinkTest
{
protected:
	void SetUp() override { recreate(congestionTimings()); }

	void queueMessages(int amount, Lane lane = Lane::Reliable)
	{
		for (int i = 0; i < amount; ++i)
			send(a, lane, 0, bytes({1}));
	}

	// One round trip without loss. Returns the number of Data packets a sent.
	size_t roundTrip()
	{
		const auto pass = take(a);
		deliverPass(b, pass);
		transfer(b, a);
		return dataSeqs(pass).size();
	}

	static std::vector<uint64_t> seqs(uint64_t first, uint64_t last)
	{
		std::vector<uint64_t> result;
		for (uint64_t seq = first; seq <= last; ++seq)
			result.push_back(seq);
		return result;
	}
};


TEST_F(CongestionTest, FirstPass_SendsTheInitialWindow)
{
	queueMessages(100);

	EXPECT_EQ(dataSeqs(take(a)).size(), InitialCongestionWindow);
	EXPECT_EQ(a.inFlightCount(), InitialCongestionWindow);
	EXPECT_TRUE(take(a).empty()) << "More only goes out as Acks come in";
}


TEST_F(CongestionTest, Window_GrowsByOnePacketPerRoundTrip)
{
	queueMessages(1000);

	EXPECT_EQ(roundTrip(), 32u);
	EXPECT_EQ(roundTrip(), 33u);
	EXPECT_EQ(roundTrip(), 34u);
	EXPECT_EQ(a.congestionWindow(), 35u);
}


TEST_F(CongestionTest, Window_NeverExceedsItsMaximum)
{
	timings.initialCwnd = MaxCongestionWindow - 2;
	recreate(timings);
	queueMessages(5000);

	for (int i = 0; i < 10; ++i)
		roundTrip();

	EXPECT_EQ(a.congestionWindow(), MaxCongestionWindow);
	EXPECT_EQ(roundTrip(), MaxCongestionWindow);
}


TEST_F(CongestionTest, FastRetransmit_ResendsAHoleWithoutWaitingForTheRto)
{
	queueMessages(10);

	const auto pass = take(a);
	ASSERT_EQ(pass.size(), 10u);

	// Seq 3 is lost, everything behind it arrives
	deliverPass(b, pass, seqOf(Lane::Reliable, 3));
	transfer(b, a);

	const auto repair = take(a);
	EXPECT_EQ(dataSeqs(repair), (std::vector<uint64_t>{3})) << "Seven later packets were acknowledged: seq 3 is resent long before its timeout";
	EXPECT_EQ(a.stats().fastRetransmissions, 1u);
	EXPECT_EQ(a.stats().retransmissions, 1u);
	EXPECT_EQ(a.congestionWindow(), InitialCongestionWindow / 2) << "A loss halves the window";

	deliverPass(b, repair);
	settle();
	EXPECT_EQ(atB.size(), 10u);
	EXPECT_FALSE(pending(a));
}


TEST_F(CongestionTest, FastRetransmit_ToleratesSlightReordering)
{
	queueMessages(4);

	const auto pass = take(a);
	ASSERT_EQ(pass.size(), 4u);

	// Seq 2 is missing, but only two later packets were acknowledged: too few to call it lost
	deliverPass(b, pass, seqOf(Lane::Reliable, 2));
	transfer(b, a);

	advance(5ms);

	EXPECT_TRUE(dataSeqs(take(a)).empty());
	EXPECT_EQ(a.congestionWindow(), InitialCongestionWindow);

	// It really is lost: the timeout takes care of it
	advance(95ms);
	settle();

	EXPECT_EQ(atB.size(), 4u);
	EXPECT_EQ(a.stats().retransmissions, 1u);
	EXPECT_EQ(a.stats().fastRetransmissions, 0u);
}


TEST_F(CongestionTest, SeveralLossesOfOneRound_ReduceTheWindowOnce)
{
	queueMessages(16);

	const auto pass = take(a);
	deliverPass(b, pass, [](const PacketHeader &header) { return header.seq == 2 || header.seq == 5 || header.seq == 9; });
	transfer(b, a);

	EXPECT_EQ(a.congestionWindow(), InitialCongestionWindow / 2) << "Three losses of the same burst are one congestion signal";
	EXPECT_EQ(dataSeqs(take(a)), (std::vector<uint64_t>{2, 5, 9})) << "All of them are resent, the oldest first";
}


TEST_F(CongestionTest, Timeout_HalvesTheWindowOncePerRound)
{
	queueMessages(100);
	take(a); // all 32 are lost

	advance(100ms);

	EXPECT_EQ(a.congestionWindow(), InitialCongestionWindow / 2) << "32 packets timed out together: one signal, one reduction";

	const auto retry = take(a);
	EXPECT_EQ(dataSeqs(retry), seqs(1, 16)) << "The oldest lost packets go first, as many as the window allows";
	EXPECT_EQ(a.stats().fastRetransmissions, 0u);

	// Acks come back: the window opens again and everything gets through
	deliverPass(b, retry);
	transfer(b, a);

	for (int i = 0; i < 200 && pending(a); ++i)
		roundTrip();

	EXPECT_EQ(atB.size(), 100u);
}


TEST_F(CongestionTest, Window_NeverFallsBelowItsMinimum)
{
	queueMessages(100);

	// Three rounds in which nothing gets through
	for (int round = 0; round < 3; ++round)
	{
		take(a);
		advance(400ms);
	}

	EXPECT_EQ(a.congestionWindow(), MinCongestionWindow);
	EXPECT_EQ(dataSeqs(take(a)).size(), MinCongestionWindow);
	EXPECT_FALSE(failed(a));
}


TEST_F(CongestionTest, QueuedFragment_DoesNotTimeOutBeforeItWasSent)
{
	queueMessages(40);

	EXPECT_EQ(dataSeqs(take(a)).size(), 32u);

	// The Acks take longer than any timeout. What was not sent yet has no timer running.
	now += 1s;
	a.onTimer(now);

	EXPECT_EQ(a.stats().retransmissions, 0u);
	EXPECT_EQ(dataSeqs(take(a)), seqs(1, 16)) << "Only what was really sent timed out";
	EXPECT_EQ(a.stats().dataSent, 32u);
	EXPECT_EQ(a.stats().retransmissions, 16u);
}


TEST_F(CongestionTest, ControlLoss_DoesNotReduceTheWindow)
{
	// A control signal is lost and repaired by its timeout
	send(a, Lane::Control, 0, bytes({42}));
	take(a);
	advance(100ms);
	settle();

	ASSERT_EQ(atB.size(), 1u);
	EXPECT_EQ(a.stats().retransmissions, 1u);
	EXPECT_EQ(a.congestionWindow(), InitialCongestionWindow) << "The window is about Reliable and Bulk only";

	// Neither the lost signal nor one that is on the wire right now takes room in that window
	send(a, Lane::Control, 0, bytes({43}));
	queueMessages(100);

	const auto pass = take(a);
	EXPECT_EQ(dataSeqs(pass, Lane::Control).size(), 1u);
	EXPECT_EQ(dataSeqs(pass, Lane::Reliable).size(), InitialCongestionWindow);
}


// ---------------------------------------------------------------------------
// Pausing a lane
// ---------------------------------------------------------------------------

class PauseTest : public ReliableLinkTest
{
protected:
	// B's application stopped taking messages, and A learned about it with the Ack of one message
	void pauseReliable()
	{
		b.setPaused(true);

		send(a, Lane::Reliable, 0, bytes({0}));
		transfer(a, b);

		pausedAck = take(b);
		ASSERT_EQ(acks(pausedAck).size(), 1u);
		ASSERT_TRUE(acks(pausedAck)[0].paused) << "The receiver asks for a pause";
		deliverPass(a, pausedAck);
	}

	Pass pausedAck;
};


TEST_F(PauseTest, PausedReceiver_HoldsReliableAndBulkBack)
{
	pauseReliable();

	for (uint8_t i = 0; i < 100; ++i)
		send(a, Lane::Reliable, 0, bytes({i}));
	send(a, Lane::Bulk, 0, bytes({7}));

	EXPECT_TRUE(dataSeqs(take(a), Lane::Reliable).empty()) << "Nothing goes out on a lane the remote paused";

	// Control signals are not affected
	send(a, Lane::Control, 0, bytes({42}));
	EXPECT_EQ(dataSeqs(take(a), Lane::Control).size(), 1u);

	// Far longer than the peer timeout: nothing but pings and their answers
	for (int i = 0; i < 50; ++i)
	{
		settle();
		advance(100ms);
		ASSERT_FALSE(failed(a)) << "A paused lane is not a failed link";
	}

	EXPECT_EQ(std::ranges::count_if(atB, [](const auto &message) { return message.lane == Lane::Reliable; }), 1);

	// The application catches up: B says so without being asked
	b.setPaused(false);

	const auto resume = take(b);
	ASSERT_EQ(acks(resume).size(), 2u) << "One Ack for every lane that was paused";
	EXPECT_EQ(acks(resume)[0].lane, Lane::Reliable);
	EXPECT_FALSE(acks(resume)[0].paused);
	EXPECT_EQ(acks(resume)[1].lane, Lane::Bulk);
	EXPECT_FALSE(acks(resume)[1].paused);
	deliverPass(a, resume);

	settle();

	EXPECT_FALSE(pending(a));
	EXPECT_EQ(std::ranges::count_if(atB, [](const auto &message) { return message.lane == Lane::Reliable; }), 101)
		<< "Nothing was dropped on the way: pausing only slows the sender down";
	EXPECT_EQ(std::ranges::count_if(atB, [](const auto &message) { return message.lane == Lane::Bulk; }), 1);
}


TEST_F(PauseTest, PausedLane_IsAskedAgainLessAndLessOften)
{
	pauseReliable();
	send(a, Lane::Reliable, 0, bytes({1}));

	// The first probe after one retransmission timeout (100 ms here), then the interval doubles up to the largest timeout
	std::vector<std::chrono::milliseconds> probes;

	for (auto elapsed = 10ms; elapsed <= 1000ms; elapsed += 10ms)
	{
		advance(10ms);

		const auto pass = take(a);
		if (count(pass, PacketKind::Ping) > 0)
			probes.push_back(elapsed);

		// Every Ping is answered, still with a pause
		deliverPass(b, pass);
		const auto answer = take(b);
		EXPECT_EQ(acks(answer).size(), count(pass, PacketKind::Ping));
		deliverPass(a, answer);
	}

	EXPECT_EQ(probes, (std::vector<std::chrono::milliseconds>{100ms, 300ms, 700ms}));
	EXPECT_EQ(a.stats().dataSent, 1u) << "No data went out while the lane was paused";
}


TEST_F(PauseTest, LostResumeAck_IsRecoveredByTheLanePing)
{
	pauseReliable();
	send(a, Lane::Reliable, 0, bytes({1}));

	b.setPaused(false);
	take(b); // the Ack that says so is lost

	EXPECT_TRUE(dataSeqs(take(a)).empty()) << "A still believes the lane is paused";

	advance(100ms);

	const auto probe = take(a);
	ASSERT_EQ(count(probe, PacketKind::Ping), 1u);
	deliverPass(b, probe);
	transfer(b, a);

	settle();
	EXPECT_EQ(atB.size(), 2u) << "The answer to the Ping carried the news";
	EXPECT_FALSE(pending(a));
}


TEST_F(PauseTest, StaleAckWithEqualCumulative_DoesNotRepause)
{
	b.setPaused(true);

	send(a, Lane::Reliable, 0, bytes({0}));
	transfer(a, b);
	const auto paused = take(b); // delayed on its way

	b.setPaused(false);
	const auto resumed = take(b);

	ASSERT_EQ(acks(paused)[0].seq, acks(resumed)[0].seq) << "Both Acks name the same seq: only their serial tells which one is newer";

	// The later Ack overtakes the earlier one
	deliverPass(a, resumed);
	deliverPass(a, paused);

	send(a, Lane::Reliable, 0, bytes({1}));
	EXPECT_EQ(dataSeqs(take(a)), (std::vector<uint64_t>{2})) << "The Ack that arrived last is not the one that was sent last";
}


TEST_F(PauseTest, AssemblyBudget_PausesOnlyThatLane)
{
	// Room for one 8 KB message at a time
	AssemblyBudget budget(10'000);
	b = ReliableLink(timings, 0xBBBB0001, &budget);

	send(a, Lane::Bulk, 1, pattern(8000));
	send(a, Lane::Reliable, 2, pattern(8000));

	const auto pass = take(a);

	// The blob starts first and takes the room
	deliver(b, only(pass, Lane::Bulk).front());
	EXPECT_EQ(budget.used(), 8000u);

	deliverPass(b, only(pass, Lane::Reliable));

	auto answer = acks(take(b));
	ASSERT_EQ(answer.size(), 2u);
	EXPECT_EQ(answer[0].lane, Lane::Reliable);
	EXPECT_TRUE(answer[0].paused) << "No room for a second message: the sender has to wait";
	EXPECT_EQ(answer[0].seq, 0u) << "Its first fragment is not acknowledged";
	EXPECT_FALSE(answer[1].paused) << "The lane that got the room goes on";

	// The blob completes and frees the room
	Pass rest = only(pass, Lane::Bulk);
	rest.erase(rest.begin());
	deliverPass(b, rest);

	ASSERT_EQ(atB.size(), 1u);
	EXPECT_EQ(atB[0].tag, 1u);
	EXPECT_EQ(budget.used(), 0u);

	// The paused lane is asked again, learns that there is room and repeats what was refused
	transfer(b, a);
	for (int i = 0; i < 20 && pending(a); ++i)
	{
		advance(50ms);
		settle();
	}

	ASSERT_EQ(atB.size(), 2u);
	EXPECT_EQ(atB[1].tag, 2u);
	EXPECT_EQ(atB[1].body, pattern(8000));
	EXPECT_EQ(budget.used(), 0u);
	EXPECT_FALSE(failed(a));
}

} // namespace ChannelTests
