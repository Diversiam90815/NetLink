#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <optional>
#include <random>
#include <vector>

#include "Channel/Reliability/ReliableLink.h"

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

	// What a DataAck or AckAck says
	struct Ack
	{
		PacketKind			  kind{PacketKind::DataAck};
		ChannelId			  channel{ChannelId::Control};
		uint64_t			  seq{0};
		uint16_t			  window{0}; // DataAck only
		std::vector<SeqRange> ranges;
	};

	static ReliabilityConfig defaultConfig()
	{
		ReliabilityConfig config;
		config.initialRto		 = 100ms;
		config.minRto			 = 20ms;
		config.maxRto			 = 400ms;
		config.failureTimeout	 = 2s;
		config.maxAckRetransmits = 3;

		// These tests are about the protocol, not about congestion: the window never limits
		config.minCongestionWindow	   = 4 * WindowSize;
		config.initialCongestionWindow = 4 * WindowSize;
		config.maxCongestionWindow	   = 4 * WindowSize;
		return config;
	}

	// The fixture's timers with the real congestion window (16 packets, adapting)
	static ReliabilityConfig congestionConfig()
	{
		ReliabilityConfig config	   = defaultConfig();
		config.minCongestionWindow	   = ReliabilityConfig{}.minCongestionWindow;
		config.initialCongestionWindow = ReliabilityConfig{}.initialCongestionWindow;
		config.maxCongestionWindow	   = ReliabilityConfig{}.maxCongestionWindow;
		return config;
	}

	ReliabilityConfig			  config = defaultConfig();
	ReliableLink				  a{config, 0xAAAA0001};
	ReliableLink				  b{config, 0xBBBB0001};
	ReliableLink::TimePoint		  now = ReliableLink::Clock::now();

	std::vector<DeliveredMessage> atA;
	std::vector<DeliveredMessage> atB;

	void						  recreate(const ReliabilityConfig &newConfig)
	{
		config = newConfig;
		a	   = ReliableLink(config, 0xAAAA0001);
		b	   = ReliableLink(config, 0xBBBB0001);
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
	size_t transfer(ReliableLink &from, ReliableLink &to, const Drop &drop = {}) { return deliverPass(to, from.takeOutgoing(now), drop); }

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

	void advance(std::chrono::milliseconds step)
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

	static Drop kind(PacketKind packetKind)
	{
		return [packetKind](const PacketHeader &header) { return header.flags.kind() == packetKind; };
	}

	// Seqs of the Data packets in one send pass, in order
	static std::vector<uint64_t> dataSeqs(const Pass &pass, std::optional<ChannelId> channel = {})
	{
		std::vector<uint64_t> seqs;
		for (const auto &datagram : pass)
		{
			const auto all	  = datagram.bytes();
			const auto packet = decodePacket(all);

			if (packet && packet->header.flags.kind() == PacketKind::Data && (!channel || packet->header.flags.channel() == *channel))
				seqs.push_back(packet->header.seq);
		}
		return seqs;
	}

	// The acknowledgements of one kind in a send pass
	static std::vector<Ack> acks(const Pass &pass, PacketKind packetKind)
	{
		std::vector<Ack> result;
		for (const auto &datagram : pass)
		{
			const auto all	  = datagram.bytes();
			const auto packet = decodePacket(all);

			if (!packet || packet->header.flags.kind() != packetKind)
				continue;

			Ack ack;
			ack.kind	= packetKind;
			ack.channel = packet->header.flags.channel();
			ack.seq		= packet->header.seq;

			auto body = packet->body;

			if (packetKind == PacketKind::DataAck)
			{
				ack.window = readUint16(body.data());
				body	   = body.subspan(AckWindowFieldSize);
			}

			ack.ranges = decodeRanges(body).value_or(std::vector<SeqRange>{});
			result.push_back(std::move(ack));
		}
		return result;
	}

	bool failed(ReliableLink &link)
	{
		bool result = false;
		for (const auto event : link.takeEvents())
			result |= event == LinkEvent::Failed;
		return result;
	}
};


// ---------------------------------------------------------------------------
// The three-way exchange
// ---------------------------------------------------------------------------

TEST_F(ReliableLinkTest, HappyPath_DataAckAckAck)
{
	ASSERT_EQ(a.queueReliable(ChannelId::Application, 7, bytes({1, 2, 3})), PushResult::Accepted);
	EXPECT_TRUE(a.hasPendingReliable());

	EXPECT_EQ(transfer(a, b), 1u) << "One Data packet";
	EXPECT_EQ(a.inFlightCount(), 1u);
	ASSERT_EQ(atB.size(), 1u);
	EXPECT_EQ(atB[0].body, bytes({1, 2, 3}));
	EXPECT_EQ(atB[0].channel, ChannelId::Application);
	EXPECT_EQ(atB[0].tag, 7u);

	EXPECT_EQ(transfer(b, a), 1u) << "One DataAck";
	EXPECT_EQ(a.inFlightCount(), 0u);
	EXPECT_FALSE(a.hasPendingReliable());

	EXPECT_EQ(transfer(a, b), 1u) << "One AckAck";

	EXPECT_EQ(a.stats().dataSent, 1u);
	EXPECT_EQ(b.stats().dataAcksSent, 1u);
	EXPECT_EQ(a.stats().ackAcksSent, 1u);

	// Nothing is left to retransmit on either side, and nothing to fail for
	advance(5s);
	EXPECT_TRUE(a.takeOutgoing(now).empty());
	EXPECT_TRUE(b.takeOutgoing(now).empty());
	EXPECT_EQ(a.stats().retransmissions, 0u);
	EXPECT_FALSE(failed(a));
}


TEST_F(ReliableLinkTest, OneDataAckAndOneAckAck_CoverAWholeBatch)
{
	for (uint8_t i = 0; i < 50; ++i)
		a.queueReliable(ChannelId::Application, 0, bytes({i}));

	EXPECT_EQ(transfer(a, b), 50u);
	ASSERT_EQ(atB.size(), 50u);

	const auto fromB	= b.takeOutgoing(now);
	const auto dataAcks = acks(fromB, PacketKind::DataAck);

	ASSERT_EQ(fromB.size(), 1u) << "Everything that arrived since the last pass is acknowledged with one datagram";
	ASSERT_EQ(dataAcks.size(), 1u);
	EXPECT_EQ(dataAcks[0].channel, ChannelId::Application);
	EXPECT_EQ(dataAcks[0].seq, 50u) << "The header names the highest seq received without a gap";
	EXPECT_EQ(dataAcks[0].ranges, (std::vector<SeqRange>{{1, 50}})) << "Every single seq is acknowledged";
	EXPECT_EQ(dataAcks[0].window, WindowSize);

	deliverPass(a, fromB);
	EXPECT_FALSE(a.hasPendingReliable());

	const auto fromA   = a.takeOutgoing(now);
	const auto ackAcks = acks(fromA, PacketKind::AckAck);

	ASSERT_EQ(fromA.size(), 1u);
	ASSERT_EQ(ackAcks.size(), 1u);
	EXPECT_EQ(ackAcks[0].seq, 50u);
	EXPECT_EQ(ackAcks[0].ranges, (std::vector<SeqRange>{{1, 50}})) << "Every acknowledged seq is confirmed";

	// The receiver is done as well: nothing is listed again
	deliverPass(b, fromA);
	advance(1s);
	EXPECT_TRUE(b.takeOutgoing(now).empty());
}


TEST_F(ReliableLinkTest, LostData_IsRetransmittedAfterTheRto)
{
	a.queueReliable(ChannelId::Application, 0, bytes({7}));
	a.takeOutgoing(now); // lost on the wire

	advance(99ms);
	EXPECT_TRUE(a.takeOutgoing(now).empty()) << "Not before the RTO";

	advance(1ms);
	settle();

	ASSERT_EQ(atB.size(), 1u);
	EXPECT_EQ(atB[0].body, bytes({7}));
	EXPECT_EQ(a.stats().retransmissions, 1u);
	EXPECT_FALSE(a.hasPendingReliable());
}


TEST_F(ReliableLinkTest, LostDataAck_DataIsResentButDeliveredOnce)
{
	a.queueReliable(ChannelId::Application, 0, bytes({7}));
	transfer(a, b);
	b.takeOutgoing(now); // the DataAck is lost
	ASSERT_EQ(atB.size(), 1u);

	advance(100ms);
	settle();

	EXPECT_EQ(atB.size(), 1u) << "A retransmitted Data packet must not be delivered twice";
	EXPECT_GE(b.stats().duplicatesReceived, 1u);
	EXPECT_FALSE(a.hasPendingReliable()) << "The re-sent DataAck completes the message";
}


TEST_F(ReliableLinkTest, LostDataAck_IsCoveredByTheNextOne)
{
	a.queueReliable(ChannelId::Application, 0, bytes({1}));
	transfer(a, b);
	b.takeOutgoing(now); // the DataAck for seq 1 is lost

	a.queueReliable(ChannelId::Application, 0, bytes({2}));
	transfer(a, b);

	const auto fromB	= b.takeOutgoing(now);
	const auto dataAcks = acks(fromB, PacketKind::DataAck);
	ASSERT_EQ(dataAcks.size(), 1u);
	EXPECT_EQ(dataAcks[0].ranges, (std::vector<SeqRange>{{2, 1}}));
	EXPECT_EQ(dataAcks[0].seq, 2u) << "... but it also says that everything up to seq 2 arrived";

	deliverPass(a, fromB);

	EXPECT_FALSE(a.hasPendingReliable()) << "Seq 1 is acknowledged without its own DataAck";
	EXPECT_EQ(a.stats().retransmissions, 0u) << "... and without sending it again";
}


TEST_F(ReliableLinkTest, LostAckAck_DataAckIsResentAndConfirmedAgain)
{
	a.queueReliable(ChannelId::Application, 0, bytes({7}));
	transfer(a, b);
	transfer(b, a);
	a.takeOutgoing(now); // the AckAck is lost
	EXPECT_EQ(b.stats().dataAcksSent, 1u);

	advance(100ms);

	const auto again = acks(b.takeOutgoing(now), PacketKind::DataAck);
	ASSERT_EQ(again.size(), 1u) << "Without AckAck the receiver resends its DataAck";
	EXPECT_EQ(again[0].ranges, (std::vector<SeqRange>{{1, 1}}));

	// Hand it over by replaying what b just sent
	PacketHeader header;
	header.flags	   = PacketFlags::ack(PacketKind::DataAck, ChannelId::Application);
	header.srcStreamID = b.localStreamID();
	header.dstStreamID = a.localStreamID();
	header.seq		   = again[0].seq;

	std::vector<uint8_t> body(AckWindowFieldSize);
	writeUint16(body.data(), again[0].window);
	appendRange(body, again[0].ranges[0]);

	const auto datagram = encodePacket(header, body);
	a.onPacket(*decodePacket(datagram), now);

	settle();
	EXPECT_EQ(a.stats().ackAcksSent, 2u) << "An already completed seq is confirmed again";

	// The AckAck arrived: the receiver stops resending
	advance(1s);
	advance(1s);
	EXPECT_TRUE(b.takeOutgoing(now).empty());
	EXPECT_EQ(b.stats().dataAcksSent, 2u);
	EXPECT_EQ(atB.size(), 1u);
}


TEST_F(ReliableLinkTest, LostAckAck_IsCoveredByTheNextOne)
{
	a.queueReliable(ChannelId::Application, 0, bytes({1}));
	transfer(a, b);
	transfer(b, a);
	a.takeOutgoing(now); // the AckAck for seq 1 is lost

	a.queueReliable(ChannelId::Application, 0, bytes({2}));
	transfer(a, b);
	transfer(b, a);

	const auto fromA   = a.takeOutgoing(now);
	const auto ackAcks = acks(fromA, PacketKind::AckAck);
	ASSERT_EQ(ackAcks.size(), 1u);
	EXPECT_EQ(ackAcks[0].seq, 2u) << "The header confirms every DataAck up to seq 2";

	deliverPass(b, fromA);

	advance(1s);
	EXPECT_TRUE(b.takeOutgoing(now).empty()) << "Seq 1 is confirmed without its own AckAck: nothing to resend";
	EXPECT_EQ(b.stats().dataAcksSent, 2u);
}


TEST_F(ReliableLinkTest, ReceiverStopsResendingDataAckAfterItsLimit)
{
	a.queueReliable(ChannelId::Application, 0, bytes({7}));
	transfer(a, b);
	transfer(b, a);
	a.takeOutgoing(now); // every AckAck gets lost from here on

	for (int i = 0; i < 20; ++i)
	{
		advance(500ms);
		b.takeOutgoing(now);
	}

	EXPECT_EQ(b.stats().dataAcksSent, 1u + static_cast<uint64_t>(config.maxAckRetransmits)) << "DataAck resends are bounded";
}


TEST_F(ReliableLinkTest, ReorderedData_IsDeliveredInOrder)
{
	a.queueReliable(ChannelId::Application, 0, bytes({1}));
	a.queueReliable(ChannelId::Application, 0, bytes({2}));
	a.queueReliable(ChannelId::Application, 0, bytes({3}));

	const auto datagrams = a.takeOutgoing(now);
	ASSERT_EQ(datagrams.size(), 3u);

	deliver(b, datagrams[2]);
	collect();
	EXPECT_TRUE(atB.empty()) << "Seq 3 waits for the gap before it to close";

	const auto early = acks(b.takeOutgoing(now), PacketKind::DataAck);
	ASSERT_EQ(early.size(), 1u);
	EXPECT_EQ(early[0].seq, 0u) << "Nothing arrived in order yet";
	EXPECT_EQ(early[0].ranges, (std::vector<SeqRange>{{3, 1}})) << "A buffered packet is acknowledged right away";

	deliver(b, datagrams[0]);
	deliver(b, datagrams[1]);
	collect();

	ASSERT_EQ(atB.size(), 3u);
	EXPECT_EQ(atB[0].body, bytes({1}));
	EXPECT_EQ(atB[1].body, bytes({2}));
	EXPECT_EQ(atB[2].body, bytes({3}));

	const auto late = acks(b.takeOutgoing(now), PacketKind::DataAck);
	ASSERT_EQ(late.size(), 1u);
	EXPECT_EQ(late[0].seq, 3u);
	EXPECT_EQ(late[0].ranges, (std::vector<SeqRange>{{1, 2}}));
}


TEST_F(ReliableLinkTest, DuplicatedDatagram_IsDeliveredOnce)
{
	a.queueReliable(ChannelId::Application, 0, bytes({1}));
	const auto datagrams = a.takeOutgoing(now);

	deliver(b, datagrams[0]);
	b.takeOutgoing(now);

	deliver(b, datagrams[0]);
	collect();

	EXPECT_EQ(atB.size(), 1u);
	EXPECT_EQ(b.stats().duplicatesReceived, 1u);

	const auto again = acks(b.takeOutgoing(now), PacketKind::DataAck);
	ASSERT_EQ(again.size(), 1u) << "The duplicate is acknowledged too, its sender may have missed the first DataAck";
	EXPECT_EQ(again[0].ranges, (std::vector<SeqRange>{{1, 1}}));
}


TEST_F(ReliableLinkTest, BrokenAcknowledgements_AreIgnored)
{
	a.queueReliable(ChannelId::Application, 0, bytes({1}));
	transfer(a, b);
	b.takeOutgoing(now);

	PacketHeader header;
	header.flags	   = PacketFlags::ack(PacketKind::DataAck, ChannelId::Application);
	header.srcStreamID = b.localStreamID();
	header.dstStreamID = a.localStreamID();
	header.seq		   = 1;

	const auto withoutWindow = encodePacket(header, bytes({1}));
	a.onPacket(*decodePacket(withoutWindow), now);
	EXPECT_EQ(a.inFlightCount(), 1u) << "A DataAck without its window field";

	const auto halfARange = encodePacket(header, bytes({4, 0, 1, 2, 3}));
	a.onPacket(*decodePacket(halfARange), now);
	EXPECT_EQ(a.inFlightCount(), 1u) << "A DataAck whose ranges are cut off";

	header.flags			 = PacketFlags::ack(PacketKind::DataAck, ChannelId::Control);
	const auto otherChannel = encodePacket(header, bytes({4, 0}));
	a.onPacket(*decodePacket(otherChannel), now);
	EXPECT_EQ(a.inFlightCount(), 1u) << "A DataAck for a channel nothing was sent on";
}


TEST_F(ReliableLinkTest, AcknowledgementOfUnsentSeqs_ReleasesOnlyWhatWasSent)
{
	a.queueReliable(ChannelId::Application, 0, bytes({1}));
	a.takeOutgoing(now);

	PacketHeader header;
	header.flags	   = PacketFlags::ack(PacketKind::DataAck, ChannelId::Application);
	header.srcStreamID = b.localStreamID();
	header.seq		   = 1'000'000; // far beyond anything a sent

	std::vector<uint8_t> body(AckWindowFieldSize);
	writeUint16(body.data(), static_cast<uint16_t>(WindowSize));
	appendRange(body, {5'000'000, 60'000});

	const auto datagram = encodePacket(header, body);
	a.onPacket(*decodePacket(datagram), now);

	EXPECT_FALSE(a.hasPendingReliable()) << "Seq 1 is covered";

	// The stream continues with seq 2 as if nothing happened
	a.queueReliable(ChannelId::Application, 0, bytes({2}));
	EXPECT_EQ(dataSeqs(a.takeOutgoing(now)), (std::vector<uint64_t>{2}));
}


// ---------------------------------------------------------------------------
// Failure and restart
// ---------------------------------------------------------------------------

TEST_F(ReliableLinkTest, UnacknowledgedData_FailsTheLinkAfterTheFailureTimeout)
{
	const uint32_t streamID = a.localStreamID();
	const auto	   started	= now;

	a.queueReliable(ChannelId::Application, 0, bytes({1}));

	bool linkFailed = false;
	for (int i = 0; i < 100 && !linkFailed; ++i)
	{
		a.takeOutgoing(now); // everything is lost
		advance(100ms);
		linkFailed = failed(a);
	}

	ASSERT_TRUE(linkFailed);
	EXPECT_GE(now - started, config.failureTimeout) << "Not before the timeout";
	EXPECT_LE(now - started, config.failureTimeout + 200ms);
	EXPECT_GT(a.stats().retransmissions, 0u) << "It kept trying until then";
	EXPECT_FALSE(a.hasPendingReliable()) << "The failed stream is discarded";
	EXPECT_NE(a.localStreamID(), streamID) << "A new stream needs a new stream ID";
}


TEST_F(ReliableLinkTest, SlowProgress_DoesNotFailTheLink)
{
	for (uint8_t i = 0; i < 40; ++i)
		a.queueReliable(ChannelId::Application, 0, bytes({i}));

	// A path that is nearly dead: one Data packet gets through per timeout. That is slow, but it is not a failed link.
	for (int i = 0; i < 200 && a.hasPendingReliable(); ++i)
	{
		bool	   passed	= false;
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

	EXPECT_FALSE(a.hasPendingReliable());
	EXPECT_EQ(atB.size(), 40u) << "Everything arrives, long after the failure timeout would have passed";
}


TEST_F(ReliableLinkTest, AfterFailure_BothSidesResynchronise)
{
	// Establish both streams first
	a.queueReliable(ChannelId::Application, 0, bytes({1}));
	b.queueReliable(ChannelId::Application, 0, bytes({2}));
	settle();
	ASSERT_EQ(atB.size(), 1u);
	ASSERT_EQ(atA.size(), 1u);

	// A's next message never gets through: A fails
	a.queueReliable(ChannelId::Application, 0, bytes({3}));
	bool linkFailed = false;
	for (int i = 0; i < 100 && !linkFailed; ++i)
	{
		a.takeOutgoing(now);
		b.takeOutgoing(now);
		advance(100ms);
		linkFailed = failed(a);
	}
	ASSERT_TRUE(linkFailed);

	// The network recovers
	a.queueReliable(ChannelId::Application, 0, bytes({4}));
	settle();

	ASSERT_EQ(atB.size(), 2u);
	EXPECT_EQ(atB[1].body, bytes({4}));

	auto eventsB = b.takeEvents();
	ASSERT_EQ(eventsB.size(), 1u);
	EXPECT_EQ(eventsB[0], LinkEvent::PeerRestarted) << "The new stream ID tells B to reset as well";

	b.queueReliable(ChannelId::Application, 0, bytes({5}));
	settle();
	ASSERT_EQ(atA.size(), 2u);
	EXPECT_EQ(atA[1].body, bytes({5})) << "B's stream to A continues after the reset";
}


TEST_F(ReliableLinkTest, PeerRestart_ResetsTheLink)
{
	a.queueReliable(ChannelId::Application, 0, bytes({1}));
	settle();
	ASSERT_EQ(a.remoteStreamID(), b.localStreamID());

	// B restarts: a brand-new link with a new stream ID starts at seq 1 again
	ReliableLink restarted(config, 0xBBBB0002);
	a.queueReliable(ChannelId::Application, 0, bytes({2})); // queued towards the old B
	a.takeOutgoing(now);

	restarted.queueReliable(ChannelId::Control, 0, bytes({9}));
	deliverPass(a, restarted.takeOutgoing(now));

	auto events = a.takeEvents();
	ASSERT_EQ(events.size(), 1u);
	EXPECT_EQ(events[0], LinkEvent::PeerRestarted);
	EXPECT_EQ(a.remoteStreamID(), 0xBBBB0002u);
	EXPECT_FALSE(a.hasPendingReliable()) << "Messages for the old stream are dropped";

	ASSERT_EQ(atA.size(), 1u) << "seq 1 of the new stream is not mistaken for a duplicate";
	EXPECT_EQ(atA[0].body, bytes({9}));
}


TEST_F(ReliableLinkTest, PeerRestart_AbandonsAPartlyReceivedMessage)
{
	const auto big = pattern(10'000);
	a.queueReliable(ChannelId::Application, 0, big);

	auto fragments = a.takeOutgoing(now);
	ASSERT_GT(fragments.size(), 3u);
	fragments.resize(3);
	deliverPass(b, fragments);

	// A restarts in the middle of its message and sends a short one on the new stream
	ReliableLink restarted(config, 0xAAAA0002);
	restarted.queueReliable(ChannelId::Application, 0, bytes({9}));
	deliverPass(b, restarted.takeOutgoing(now));

	ASSERT_EQ(atB.size(), 1u);
	EXPECT_EQ(atB[0].body, bytes({9})) << "Nothing of the old stream's partial message may show up";
}


TEST_F(ReliableLinkTest, PacketForAnOldStream_IsDropped)
{
	a.queueReliable(ChannelId::Application, 0, bytes({1}));
	settle();

	PacketHeader header;
	header.flags	   = PacketFlags::data(ChannelId::Application, true);
	header.srcStreamID = a.localStreamID();
	header.dstStreamID = 0x12345678; // not B
	header.seq		   = 2;

	const auto datagram = encodePacket(header, bytes({5}));
	b.onPacket(*decodePacket(datagram), now);
	collect();

	EXPECT_EQ(b.stats().staleDropped, 1u);
	EXPECT_EQ(atB.size(), 1u);
	EXPECT_TRUE(b.takeOutgoing(now).empty()) << "A stale packet is not acknowledged";
}


// ---------------------------------------------------------------------------
// Windows and queues
// ---------------------------------------------------------------------------

TEST_F(ReliableLinkTest, DataBeyondTheReceiveWindow_IsNotAcknowledged)
{
	PacketHeader header;
	header.flags	   = PacketFlags::data(ChannelId::Application, true);
	header.srcStreamID = a.localStreamID();
	header.seq		   = 1 + WindowSize;

	const auto datagram = encodePacket(header, bytes({5}));
	b.onPacket(*decodePacket(datagram), now);

	EXPECT_EQ(b.stats().outOfWindowDropped, 1u);
	EXPECT_TRUE(b.takeOutgoing(now).empty());
}


TEST_F(ReliableLinkTest, SendWindowLimitsPacketsInFlight)
{
	const size_t total = WindowSize + 44;
	for (size_t i = 0; i < total; ++i)
		ASSERT_EQ(a.queueReliable(ChannelId::Application, 0, {static_cast<uint8_t>(i), static_cast<uint8_t>(i >> 8)}), PushResult::Accepted);

	const auto first = a.takeOutgoing(now);
	EXPECT_EQ(first.size(), WindowSize) << "Never more unacknowledged seqs than the receiver can buffer";
	EXPECT_EQ(a.inFlightCount(), WindowSize);
	EXPECT_EQ(a.queuedMessageCount(), 44u);
	EXPECT_TRUE(a.takeOutgoing(now).empty()) << "The window is full until acknowledgements arrive";

	deliverPass(b, first);
	settle();

	ASSERT_EQ(atB.size(), total);
	for (size_t i = 0; i < total; ++i)
		EXPECT_EQ(atB[i].body, (std::vector<uint8_t>{static_cast<uint8_t>(i), static_cast<uint8_t>(i >> 8)})) << "in order at " << i;
	EXPECT_FALSE(a.hasPendingReliable());
}


TEST_F(ReliableLinkTest, DropNewest_RejectsWhenTheQueueIsFull)
{
	config.sendQueueCapacity = 2;
	config.sendQueueOverflow = OverflowPolicy::DropNewest;
	ReliableLink link(config);

	EXPECT_TRUE(link.hasRoomFor(ChannelId::Application));
	EXPECT_EQ(link.queueReliable(ChannelId::Application, 0, bytes({2})), PushResult::Accepted);
	EXPECT_EQ(link.queueReliable(ChannelId::Application, 0, bytes({3})), PushResult::Accepted);
	EXPECT_FALSE(link.hasRoomFor(ChannelId::Application));
	EXPECT_EQ(link.queueReliable(ChannelId::Application, 0, bytes({4})), PushResult::Rejected);
	EXPECT_EQ(link.queueReliable(ChannelId::Control, 0, bytes({5})), PushResult::Accepted) << "Control signals never compete with application backpressure";

	link.takeOutgoing(now);
	EXPECT_TRUE(link.hasRoomFor(ChannelId::Application)) << "What went onto the wire left the queue";
}


TEST_F(ReliableLinkTest, DropOldest_EvictsUnsentMessagesWithoutLeavingAGap)
{
	config.sendQueueCapacity = 4;
	config.sendQueueOverflow = OverflowPolicy::DropOldest;
	a						 = ReliableLink(config, 0xAAAA0009);

	// Already on the wire: these can no longer be evicted
	for (int i = 0; i < 3; ++i)
		a.queueReliable(ChannelId::Application, 0, bytes({0}));
	const auto onTheWire = a.takeOutgoing(now);

	for (uint8_t value = 1; value <= 6; ++value)
	{
		EXPECT_TRUE(a.hasRoomFor(ChannelId::Application)) << "A queue that drops its oldest message always takes a new one";
		const auto result = a.queueReliable(ChannelId::Application, 0, bytes({value}));
		EXPECT_EQ(result, value <= 4 ? PushResult::Accepted : PushResult::EvictedOldest);
	}

	deliverPass(b, onTheWire);
	settle();

	ASSERT_EQ(atB.size(), 3u + 4u) << "Every message that was kept arrives: evicting never created a gap in the stream";
	EXPECT_EQ(atB[3].body, bytes({3})) << "1 and 2 were the oldest unsent messages";
	EXPECT_EQ(atB.back().body, bytes({6}));
	EXPECT_FALSE(a.hasPendingReliable());
}


TEST_F(ReliableLinkTest, DropQueuedApplicationMessages_KeepsControlSignalsAndWhatIsOnTheWire)
{
	for (int i = 0; i < 3; ++i)
		a.queueReliable(ChannelId::Application, 0, bytes({1}));
	const auto onTheWire = a.takeOutgoing(now);

	for (int i = 0; i < 5; ++i)
		a.queueReliable(ChannelId::Application, 0, bytes({1}));
	a.queueReliable(ChannelId::Control, 0, bytes({2}));

	a.dropQueuedApplicationMessages();
	EXPECT_EQ(a.queuedMessageCount(), 1u) << "Only the control signal is left in the queue";

	deliverPass(b, onTheWire);
	settle();

	EXPECT_EQ(atB.size(), 3u + 1u) << "Messages already on the wire still complete";
	EXPECT_FALSE(a.hasPendingReliable());
}


TEST_F(ReliableLinkTest, DropQueuedApplicationMessages_InTheMiddleOfAMessage)
{
	ReliabilityConfig narrow	   = defaultConfig();
	narrow.minCongestionWindow	   = 4;
	narrow.initialCongestionWindow = 4;
	narrow.maxCongestionWindow	   = 4;
	recreate(narrow);

	a.queueReliable(ChannelId::Application, 0, pattern(10'000)); // 9 fragments, 4 of them fit into the window
	const auto partial = a.takeOutgoing(now);
	ASSERT_EQ(partial.size(), 4u);

	a.dropQueuedApplicationMessages();
	a.queueReliable(ChannelId::Application, 5, bytes({7}));

	deliverPass(b, partial);
	settle();

	ASSERT_EQ(atB.size(), 1u) << "The message that lost its end is abandoned by the receiver";
	EXPECT_EQ(atB[0].body, bytes({7}));
	EXPECT_EQ(atB[0].tag, 5u);
	EXPECT_FALSE(a.hasPendingReliable());
}


// ---------------------------------------------------------------------------
// Channels
// ---------------------------------------------------------------------------

TEST_F(ReliableLinkTest, ControlSignal_IsNotHeldUpByAFullApplicationWindow)
{
	for (size_t i = 0; i < WindowSize + 5; ++i)
		a.queueReliable(ChannelId::Application, 0, bytes({1}));

	EXPECT_EQ(a.takeOutgoing(now).size(), WindowSize) << "The application window is full, and nothing of it arrives";

	a.queueReliable(ChannelId::Control, 0, bytes({42}));

	const auto pass = a.takeOutgoing(now);
	ASSERT_EQ(pass.size(), 1u) << "The control channel has a window of its own";
	EXPECT_EQ(dataSeqs(pass, ChannelId::Control), (std::vector<uint64_t>{1})) << "... and seqs of its own";

	deliverPass(b, pass);

	ASSERT_EQ(atB.size(), 1u) << "The receiver delivers it without waiting for the application data sent before it";
	EXPECT_EQ(atB[0].channel, ChannelId::Control);
	EXPECT_EQ(atB[0].body, bytes({42}));
}


TEST_F(ReliableLinkTest, ControlSignal_IsNotHeldUpByAFullCongestionWindow)
{
	recreate(congestionConfig());

	for (int i = 0; i < 100; ++i)
		a.queueReliable(ChannelId::Application, 0, bytes({1}));

	EXPECT_EQ(dataSeqs(a.takeOutgoing(now)).size(), config.initialCongestionWindow) << "The congestion window is full, and nothing of it is acknowledged";
	EXPECT_FALSE(a.hasOutgoing());

	a.queueReliable(ChannelId::Control, 0, bytes({42}));
	EXPECT_TRUE(a.hasOutgoing()) << "The owner of the link must learn that there is something to send";

	const auto pass = a.takeOutgoing(now);
	ASSERT_EQ(pass.size(), 1u) << "The congestion window only holds application data back";
	EXPECT_EQ(dataSeqs(pass, ChannelId::Control), (std::vector<uint64_t>{1}));

	deliverPass(b, pass);

	ASSERT_EQ(atB.size(), 1u);
	EXPECT_EQ(atB[0].channel, ChannelId::Control);
	EXPECT_EQ(atB[0].body, bytes({42}));
}


TEST_F(ReliableLinkTest, ControlLoss_KeepsAccountingBalanced)
{
	recreate(congestionConfig());

	// A control signal is lost and repaired by its timeout
	a.queueReliable(ChannelId::Control, 0, bytes({42}));
	a.takeOutgoing(now);
	advance(100ms);
	settle();

	ASSERT_EQ(atB.size(), 1u);
	EXPECT_EQ(a.stats().retransmissions, 1u);
	EXPECT_EQ(a.inFlightCount(), 0u);

	// Neither the lost signal nor one that is on the wire right now takes room in the application's window
	const size_t window = a.congestionWindow();

	a.queueReliable(ChannelId::Control, 0, bytes({43}));
	for (int i = 0; i < 100; ++i)
		a.queueReliable(ChannelId::Application, 0, bytes({1}));

	const auto pass = a.takeOutgoing(now);
	EXPECT_EQ(dataSeqs(pass, ChannelId::Control).size(), 1u);
	EXPECT_EQ(dataSeqs(pass, ChannelId::Application).size(), window) << "The whole window is available to application data";
	EXPECT_TRUE(dataSeqs(a.takeOutgoing(now)).empty()) << "... and not more than that";

	deliverPass(b, pass);
	settle();

	EXPECT_EQ(atB.size(), 1u + 1u + 100u);
	EXPECT_EQ(a.inFlightCount(), 0u);
	EXPECT_FALSE(a.hasPendingReliable());
}


TEST_F(ReliableLinkTest, ControlSignals_GoFirstInASendPass)
{
	recreate(congestionConfig());

	for (int i = 0; i < 100; ++i)
		a.queueReliable(ChannelId::Application, 0, bytes({1}));
	a.queueReliable(ChannelId::Control, 0, bytes({42}));

	const auto pass = a.takeOutgoing(now);
	ASSERT_FALSE(pass.empty());

	const auto first  = pass.front().bytes();
	const auto packet = decodePacket(first);
	ASSERT_TRUE(packet.has_value());
	EXPECT_EQ(packet->header.flags.channel(), ChannelId::Control) << "Queued last, sent first: application data cannot use up the window before it";
}


TEST_F(ReliableLinkTest, Channels_AreAcknowledgedSeparately)
{
	a.queueReliable(ChannelId::Control, 0, bytes({1}));
	a.queueReliable(ChannelId::Application, 0, bytes({2}));
	a.queueReliable(ChannelId::Application, 0, bytes({3}));
	transfer(a, b);

	const auto dataAcks = acks(b.takeOutgoing(now), PacketKind::DataAck);
	ASSERT_EQ(dataAcks.size(), 2u);

	EXPECT_EQ(dataAcks[0].channel, ChannelId::Control);
	EXPECT_EQ(dataAcks[0].ranges, (std::vector<SeqRange>{{1, 1}}));
	EXPECT_EQ(dataAcks[1].channel, ChannelId::Application);
	EXPECT_EQ(dataAcks[1].ranges, (std::vector<SeqRange>{{1, 2}}));
}


// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

TEST_F(ReliableLinkTest, LargeMessage_ArrivesAsOneMessageWithItsTag)
{
	const auto body = pattern(100 * 1024);
	ASSERT_EQ(a.queueReliable(ChannelId::Application, 0xCAFE, body), PushResult::Accepted);

	settle();

	ASSERT_EQ(atB.size(), 1u) << "Fragments are put together by the link";
	EXPECT_EQ(atB[0].body, body);
	EXPECT_EQ(atB[0].tag, 0xCAFEu);
	EXPECT_EQ(b.stats().delivered, 1u);
	EXPECT_GT(a.stats().dataSent, 80u);
}


TEST_F(ReliableLinkTest, Fragments_AreSentFromTheMessageWithoutCopyingIt)
{
	a.queueReliable(ChannelId::Application, 0, pattern(5000));

	const auto pass = a.takeOutgoing(now);
	ASSERT_EQ(pass.size(), 5u);

	for (size_t i = 0; i < pass.size(); ++i)
	{
		ASSERT_TRUE(pass[i].message) << "A fragment refers to its message";
		EXPECT_EQ(pass[i].message, pass[0].message) << "... the same one for every fragment";
		EXPECT_EQ(pass[i].offset, i * a.maxFragmentBody());
		EXPECT_TRUE(pass[i].owned.empty());
	}
}


TEST_F(ReliableLinkTest, LargeMessage_IsReassembledUnderLoss)
{
	const auto body = pattern(100 * 1024);
	ASSERT_EQ(a.queueReliable(ChannelId::Application, 3, body), PushResult::Accepted);

	std::mt19937 random(3);
	auto		 lossy = [&random](const PacketHeader &) { return std::uniform_real_distribution<double>(0.0, 1.0)(random) < 0.2; };

	for (int i = 0; i < 1000 && a.hasPendingReliable(); ++i)
	{
		transfer(a, b, lossy);
		transfer(b, a, lossy);
		advance(50ms);
	}
	settle();

	ASSERT_FALSE(a.hasPendingReliable());
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

	for (int step = 0; step < 20'000 && (queued < total || a.hasPendingReliable()); ++step)
	{
		while (queued < total && a.queuedMessageCount() < 64)
		{
			a.queueReliable(ChannelId::Application, static_cast<uint32_t>(queued), {static_cast<uint8_t>(queued), static_cast<uint8_t>(queued >> 8)});
			++queued;
		}

		// A misbehaving network in both directions
		for (auto *pair : {&a, &b})
		{
			ReliableLink &from		= *pair;
			ReliableLink &to		= pair == &a ? b : a;
			auto		  datagrams = from.takeOutgoing(now);
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
	recreate(congestionConfig());

	const auto body = pattern(512 * 1024);
	ASSERT_EQ(a.queueReliable(ChannelId::Application, 0, body), PushResult::Accepted);

	std::mt19937 random(5);
	auto		 lossy = [&random](const PacketHeader &) { return std::uniform_real_distribution<double>(0.0, 1.0)(random) < 0.2; };

	for (int i = 0; i < 50'000 && a.hasPendingReliable(); ++i)
	{
		transfer(a, b, lossy);
		transfer(b, a, lossy);
		advance(5ms);
	}
	settle();

	ASSERT_FALSE(a.hasPendingReliable());
	EXPECT_FALSE(failed(a)) << "A lossy path is slow, not broken";

	ASSERT_EQ(atB.size(), 1u);
	EXPECT_EQ(atB[0].body, body);
	EXPECT_GT(a.stats().fastRetransmissions, 0u) << "Losses must have been repaired without waiting for the timeout as well";
}


TEST_F(ReliableLinkTest, NoDatagramExceedsTheMaximumSize)
{
	a.queueReliable(ChannelId::Application, 0, std::vector<uint8_t>(10000, 1));

	const auto pass = a.takeOutgoing(now);
	ASSERT_FALSE(pass.empty());

	for (const auto &datagram : pass)
		EXPECT_LE(datagram.bytes().size(), config.maxDatagramSize);

	EXPECT_EQ(pass.front().bytes().size(), config.maxDatagramSize) << "The first fragment, which carries the tag, uses the datagram completely";
}


TEST_F(ReliableLinkTest, OversizedMessage_IsRejected)
{
	config.maxMessageSize = 1000;
	ReliableLink link(config);

	EXPECT_EQ(link.queueReliable(ChannelId::Application, 0, std::vector<uint8_t>(1001)), PushResult::Rejected);
	EXPECT_FALSE(link.hasPendingReliable());
}


TEST_F(ReliableLinkTest, RttIsSampledFromUnretransmittedPackets)
{
	a.queueReliable(ChannelId::Application, 0, bytes({1}));
	transfer(a, b);
	now += 30ms;
	transfer(b, a);

	ASSERT_TRUE(a.rtt().hasSample());
	EXPECT_EQ(a.rtt().srtt(), 30ms);
}


// ---------------------------------------------------------------------------
// Unreliable messages and heartbeats
// ---------------------------------------------------------------------------

TEST_F(ReliableLinkTest, Unreliable_StaleMessagesAreDiscarded)
{
	ASSERT_TRUE(a.sendUnreliable(ChannelId::Application, 11, bytes({1})));
	ASSERT_TRUE(a.sendUnreliable(ChannelId::Application, 12, bytes({2})));
	ASSERT_TRUE(a.sendUnreliable(ChannelId::Application, 13, bytes({3})));

	const auto datagrams = a.takeOutgoing(now);
	ASSERT_EQ(datagrams.size(), 3u);

	deliver(b, datagrams[1]);
	deliver(b, datagrams[0]); // older than what was delivered
	deliver(b, datagrams[2]);
	collect();

	ASSERT_EQ(atB.size(), 2u);
	EXPECT_EQ(atB[0].body, bytes({2}));
	EXPECT_EQ(atB[0].tag, 12u);
	EXPECT_EQ(atB[1].body, bytes({3}));
	EXPECT_EQ(atB[1].tag, 13u);
	EXPECT_TRUE(b.takeOutgoing(now).empty()) << "Unreliable messages are never acknowledged";
	EXPECT_FALSE(a.hasPendingReliable());
}


TEST_F(ReliableLinkTest, Unreliable_MustFitIntoOneDatagram)
{
	std::vector<uint8_t> fits(a.maxUnreliableBody());
	std::vector<uint8_t> tooLarge(a.maxUnreliableBody() + 1);

	EXPECT_TRUE(a.sendUnreliable(ChannelId::Application, 0, fits));
	EXPECT_FALSE(a.sendUnreliable(ChannelId::Application, 0, tooLarge));

	const auto pass = a.takeOutgoing(now);
	ASSERT_EQ(pass.size(), 1u);
	EXPECT_EQ(pass[0].bytes().size(), config.maxDatagramSize);
}


TEST_F(ReliableLinkTest, UnreliableQueue_DropsTheOldestWhenFull)
{
	for (int i = 0; i < 200; ++i)
		a.sendUnreliable(ChannelId::Application, 0, bytes({1}));

	const auto	 seqs	  = dataSeqs(a.takeOutgoing(now));
	const size_t capacity = ReliabilityConfig{}.unreliableQueueCapacity;

	ASSERT_EQ(seqs.size(), capacity) << "Only as many as the queue holds";
	EXPECT_EQ(seqs.front(), 200 - capacity + 1) << "The oldest ones were dropped";
	EXPECT_EQ(seqs.back(), 200u);
}


TEST_F(ReliableLinkTest, HeartbeatIsNeitherDeliveredNorAcknowledged)
{
	a.sendHeartbeat();
	EXPECT_TRUE(a.hasOutgoing());
	EXPECT_EQ(transfer(a, b), 1u);

	EXPECT_TRUE(atB.empty());
	EXPECT_TRUE(b.takeOutgoing(now).empty());
	EXPECT_EQ(b.remoteStreamID(), a.localStreamID()) << "A heartbeat still introduces the peer";
}


TEST_F(ReliableLinkTest, SendPass_AcknowledgementsFirstThenUnreliableThenReliable)
{
	// Something to acknowledge
	b.queueReliable(ChannelId::Application, 0, bytes({9}));
	transfer(b, a);

	a.queueReliable(ChannelId::Application, 0, bytes({1}));
	a.sendUnreliable(ChannelId::Application, 0, bytes({2}));
	a.sendHeartbeat();

	const auto pass = a.takeOutgoing(now);
	ASSERT_EQ(pass.size(), 4u);

	std::vector<DecodedPacket>		  packets;
	std::vector<std::vector<uint8_t>> storage;
	for (const auto &datagram : pass)
	{
		storage.push_back(datagram.bytes());
		packets.push_back(*decodePacket(storage.back()));
	}

	EXPECT_EQ(packets[0].header.flags.kind(), PacketKind::DataAck) << "The remote's sending depends on acknowledgements";
	EXPECT_EQ(packets[1].header.flags.kind(), PacketKind::Heartbeat);
	EXPECT_FALSE(packets[2].header.flags.isReliable()) << "Unreliable data goes before reliable data";
	EXPECT_TRUE(packets[3].header.flags.isReliable());
	EXPECT_EQ(packets[3].header.flags.kind(), PacketKind::Data);
}


// ---------------------------------------------------------------------------
// Peek and commit
// ---------------------------------------------------------------------------

TEST_F(ReliableLinkTest, Peek_IsIdempotent)
{
	b.queueReliable(ChannelId::Application, 0, bytes({9}));
	transfer(b, a); // something to acknowledge

	a.queueReliable(ChannelId::Application, 7, pattern(3000));
	a.sendUnreliable(ChannelId::Application, 8, bytes({1}));

	const auto deadline = a.nextDeadline();

	for (const SendClass sendClass : {SendClass::Application, SendClass::Unreliable})
	{
		const auto *first = a.peek(sendClass, now);
		ASSERT_NE(first, nullptr);
		const auto	offered = first->bytes();

		const auto *again	= a.peek(sendClass, now);
		ASSERT_NE(again, nullptr);
		EXPECT_EQ(again->bytes(), offered) << "Until it is committed, the same datagram is offered";
	}

	const auto *ack = a.peekAck();
	ASSERT_NE(ack, nullptr);
	const auto offeredAck = ack->bytes();
	ASSERT_NE(a.peekAck(), nullptr);
	EXPECT_EQ(a.peekAck()->bytes(), offeredAck);

	EXPECT_EQ(a.peek(SendClass::Control, now), nullptr) << "Nothing was queued there";

	EXPECT_EQ(a.inFlightCount(), 0u) << "Nothing counts as sent";
	EXPECT_EQ(a.stats().dataSent, 0u);
	EXPECT_EQ(a.queuedMessageCount(), 1u);
	EXPECT_EQ(a.nextDeadline(), deadline) << "No timer runs for a datagram that did not go out";

	settle();
	EXPECT_EQ(atB.size(), 2u) << "Everything that was only looked at still arrives";
	EXPECT_EQ(a.stats().retransmissions, 0u);
}


TEST_F(ReliableLinkTest, AbortedDatagram_LeavesNoTrace)
{
	const auto body = pattern(3000); // three fragments
	a.queueReliable(ChannelId::Application, 0, body);

	const auto *first = a.peek(SendClass::Application, now);
	ASSERT_NE(first, nullptr);
	const OutgoingDatagram sent = *first;
	a.commit(SendClass::Application, now);

	// The socket refuses the second fragment: it is not committed
	const auto *second = a.peek(SendClass::Application, now);
	ASSERT_NE(second, nullptr);
	const auto refused = second->bytes();

	EXPECT_EQ(a.inFlightCount(), 1u);
	EXPECT_EQ(a.stats().dataSent, 1u);
	EXPECT_EQ(a.queuedMessageCount(), 1u);

	now += 1ms;
	const auto pass = a.takeOutgoing(now);

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
// Congestion window
// ---------------------------------------------------------------------------

class CongestionTest : public ReliableLinkTest
{
protected:
	void SetUp() override { recreate(congestionConfig()); }

	void queueMessages(int count)
	{
		for (int i = 0; i < count; ++i)
			ASSERT_EQ(a.queueReliable(ChannelId::Application, 0, bytes({1})), PushResult::Accepted);
	}

	// One round trip without loss. Returns the number of Data packets a sent.
	size_t roundTrip()
	{
		const auto pass = a.takeOutgoing(now);
		deliverPass(b, pass);
		transfer(b, a);
		return dataSeqs(pass).size();
	}
};


TEST_F(CongestionTest, FirstPass_SendsTheInitialWindow)
{
	queueMessages(100);

	EXPECT_EQ(dataSeqs(a.takeOutgoing(now)).size(), config.initialCongestionWindow);
	EXPECT_EQ(a.inFlightCount(), config.initialCongestionWindow);
	EXPECT_TRUE(a.takeOutgoing(now).empty()) << "More only goes out as acknowledgements come in";
	EXPECT_FALSE(a.hasOutgoing());
}


TEST_F(CongestionTest, Window_DoublesPerRoundTripUntilTheFirstLoss)
{
	queueMessages(1000);

	EXPECT_EQ(roundTrip(), 16u);
	EXPECT_EQ(roundTrip(), 32u);
	EXPECT_EQ(roundTrip(), 64u);
	EXPECT_EQ(roundTrip(), 128u);
	EXPECT_EQ(a.congestionWindow(), 256u);
}


TEST_F(CongestionTest, Window_NeverExceedsItsMaximum)
{
	config.sendQueueCapacity = 20'000;
	recreate(config);
	queueMessages(20'000);

	for (int i = 0; i < 20; ++i)
		roundTrip();

	EXPECT_EQ(a.congestionWindow(), config.maxCongestionWindow);
	EXPECT_EQ(roundTrip(), config.maxCongestionWindow);
}


TEST_F(CongestionTest, Window_DoesNotGrowWhileItIsNotUsedUp)
{
	for (int i = 0; i < 100; ++i)
	{
		queueMessages(3);
		EXPECT_EQ(roundTrip(), 3u);
	}

	EXPECT_EQ(a.congestionWindow(), config.initialCongestionWindow) << "A trickle says nothing about what the path can take";
}


TEST_F(CongestionTest, FastRetransmit_ResendsAHoleWithoutWaitingForTheRto)
{
	queueMessages(10);

	const auto pass = a.takeOutgoing(now);
	ASSERT_EQ(pass.size(), 10u);

	// Seq 3 is lost, everything behind it arrives
	deliverPass(b, pass, [](const PacketHeader &header) { return header.seq == 3; });
	transfer(b, a);

	const auto repair = a.takeOutgoing(now);
	EXPECT_EQ(dataSeqs(repair), (std::vector<uint64_t>{3})) << "Seven later packets were acknowledged: seq 3 is resent long before its timeout";
	EXPECT_EQ(a.stats().fastRetransmissions, 1u);
	EXPECT_EQ(a.stats().retransmissions, 1u);
	EXPECT_EQ(a.congestionWindow(), config.initialCongestionWindow / 2) << "A loss halves the window";

	deliverPass(b, repair);
	settle();
	EXPECT_EQ(atB.size(), 10u);
	EXPECT_FALSE(a.hasPendingReliable());
}


TEST_F(CongestionTest, FastRetransmit_ToleratesSlightReordering)
{
	queueMessages(4);

	const auto pass = a.takeOutgoing(now);
	ASSERT_EQ(pass.size(), 4u);

	// Seq 2 is missing, but only two later packets were acknowledged: too few to call it lost
	deliverPass(b, pass, [](const PacketHeader &header) { return header.seq == 2; });
	transfer(b, a);

	advance(5ms);

	EXPECT_TRUE(dataSeqs(a.takeOutgoing(now)).empty());
	EXPECT_EQ(a.congestionWindow(), config.initialCongestionWindow);

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

	const auto pass = a.takeOutgoing(now);
	deliverPass(b, pass, [](const PacketHeader &header) { return header.seq == 2 || header.seq == 5 || header.seq == 9; });
	transfer(b, a);
	advance(1ms);

	EXPECT_EQ(a.congestionWindow(), config.initialCongestionWindow / 2) << "Three losses of the same burst are one congestion signal";
	EXPECT_EQ(dataSeqs(a.takeOutgoing(now)), (std::vector<uint64_t>{2, 5, 9})) << "All of them are resent, the oldest first";
}


TEST_F(CongestionTest, Timeout_StartsOverWithTheSmallestWindow)
{
	queueMessages(100);
	a.takeOutgoing(now); // all 16 are lost

	advance(100ms);

	EXPECT_EQ(a.congestionWindow(), config.minCongestionWindow) << "No acknowledgement at all: the path may be overloaded";

	const auto retry = a.takeOutgoing(now);
	EXPECT_EQ(dataSeqs(retry), (std::vector<uint64_t>{1, 2, 3, 4, 5, 6, 7, 8})) << "The oldest lost packets go first, as many as the window allows";
	EXPECT_EQ(a.stats().fastRetransmissions, 0u);

	// Acknowledgements come back: the window opens again and everything gets through
	deliverPass(b, retry);
	transfer(b, a);

	for (int i = 0; i < 200 && a.hasPendingReliable(); ++i)
		roundTrip();

	EXPECT_EQ(atB.size(), 100u);
	EXPECT_GT(a.congestionWindow(), config.minCongestionWindow);
}


TEST_F(CongestionTest, AfterALoss_TheWindowGrowsByOnePacketPerRoundTrip)
{
	queueMessages(1000);

	const auto pass = a.takeOutgoing(now);
	deliverPass(b, pass, [](const PacketHeader &header) { return header.seq == 3; });
	transfer(b, a);
	advance(1ms);
	ASSERT_EQ(a.congestionWindow(), 8u);

	// Let the loss be repaired and the window fill up again
	roundTrip();

	const size_t before = a.congestionWindow();
	for (int i = 0; i < 5; ++i)
		roundTrip();

	EXPECT_GE(a.congestionWindow(), before + 3) << "Slowly: about one packet per round trip";
	EXPECT_LE(a.congestionWindow(), before + 6) << "... not doubling anymore";
}


TEST_F(CongestionTest, QueuedFragment_DoesNotTimeOutBeforeItWasSent)
{
	queueMessages(40);

	EXPECT_EQ(dataSeqs(a.takeOutgoing(now)).size(), 16u);

	// The acknowledgements take longer than any timeout. What still waits in the queue has no timer running.
	now += 1s;
	a.onTimer(now);

	EXPECT_EQ(a.stats().retransmissions, 0u);
	EXPECT_EQ(dataSeqs(a.takeOutgoing(now)), (std::vector<uint64_t>{1, 2, 3, 4, 5, 6, 7, 8})) << "Only what was really sent timed out";
	EXPECT_EQ(a.stats().dataSent, 16u);
	EXPECT_EQ(a.stats().retransmissions, 8u);
}


// ---------------------------------------------------------------------------
// Receive window (an application that does not keep up)
// ---------------------------------------------------------------------------

TEST_F(ReliableLinkTest, PausedReceiver_HoldsTheApplicationChannelBack)
{
	for (uint8_t i = 0; i < 100; ++i)
		a.queueReliable(ChannelId::Application, 0, bytes({i}));

	// B's application stopped taking messages
	b.setApplicationReceiving(false);

	// The first packet is acknowledged with a closed window
	auto pass = a.takeOutgoing(now);
	pass.resize(1);
	deliverPass(b, pass);

	const auto fromB	= b.takeOutgoing(now);
	const auto dataAcks = acks(fromB, PacketKind::DataAck);
	ASSERT_EQ(dataAcks.size(), 1u);
	EXPECT_EQ(dataAcks[0].window, 0u) << "The receiver asks for a pause";
	deliverPass(a, fromB);

	// What was already on the wire is lost in this test. From now on a only asks again from time to time.
	advance(100ms);

	size_t sent = 0;
	for (int i = 0; i < 10; ++i)
	{
		const auto probes = a.takeOutgoing(now);
		sent += dataSeqs(probes).size();
		deliverPass(b, probes);
		transfer(b, a);
		advance(config.windowProbeInterval);
	}

	EXPECT_LE(sent, 11u) << "One packet per probe interval, although far more would fit into the congestion window";
	EXPECT_GE(sent, 9u);
	EXPECT_FALSE(failed(a)) << "A paused channel is not a failed link";

	// Control signals are not affected
	a.queueReliable(ChannelId::Control, 0, bytes({42}));
	EXPECT_EQ(dataSeqs(a.takeOutgoing(now), ChannelId::Control).size(), 1u);

	// The application catches up: the next probe's acknowledgement opens the window again
	b.setApplicationReceiving(true);

	for (int i = 0; i < 50 && a.hasPendingReliable(); ++i)
	{
		settle();
		advance(config.windowProbeInterval);
	}

	EXPECT_FALSE(a.hasPendingReliable());

	size_t application = 0;
	for (const auto &message : atB)
		application += message.channel == ChannelId::Application ? 1 : 0;
	EXPECT_EQ(application, 100u) << "Nothing was dropped on the way: pausing only slows the sender down";
}


TEST_F(ReliableLinkTest, PausedReceiver_TheNextProbeIsATimer)
{
	a.queueReliable(ChannelId::Application, 0, bytes({1}));
	a.queueReliable(ChannelId::Application, 0, bytes({2}));
	a.queueReliable(ChannelId::Application, 0, bytes({3}));

	b.setApplicationReceiving(false);

	auto pass = a.takeOutgoing(now);
	ASSERT_EQ(pass.size(), 3u);
	deliverPass(b, pass);
	transfer(b, a); // everything is acknowledged, the window is closed

	a.queueReliable(ChannelId::Application, 0, bytes({4}));
	a.queueReliable(ChannelId::Application, 0, bytes({5}));

	EXPECT_EQ(dataSeqs(a.takeOutgoing(now)), (std::vector<uint64_t>{4})) << "The first packet after the pause asks right away";
	EXPECT_TRUE(dataSeqs(a.takeOutgoing(now)).empty());

	settle(kind(PacketKind::Data)); // only acknowledgements flow

	// Seq 4 was lost. Seq 5 has to wait for the probe timer, and the link tells its owner when that is.
	ASSERT_TRUE(a.nextDeadline().has_value());
	EXPECT_LE(*a.nextDeadline(), now + config.windowProbeInterval);
}

} // namespace ChannelTests
