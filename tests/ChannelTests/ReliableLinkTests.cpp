#include <gtest/gtest.h>

#include <functional>
#include <random>
#include <vector>

#include "TestIp.h"
#include "Channel/Fragmentation/FragmentationService.h"
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

	static ReliabilityConfig defaultConfig()
	{
		ReliabilityConfig config;
		config.initialRto		 = 100ms;
		config.minRto			 = 20ms;
		config.maxRto			 = 400ms;
		config.maxRetransmits	 = 5;
		config.maxAckRetransmits = 3;
		return config;
	}

	ReliabilityConfig			 config = defaultConfig();
	ReliableLink				 a{config, 0xAAAA0001};
	ReliableLink				 b{config, 0xBBBB0001};
	ReliableLink::TimePoint		 now = ReliableLink::Clock::now();

	std::vector<DeliveredPacket> atA;
	std::vector<DeliveredPacket> atB;

	// Carries everything `from` produced so far to `to`. Returns the number of datagrams that arrived.
	size_t						 transfer(ReliableLink &from, ReliableLink &to, const Drop &drop = {})
	{
		size_t arrived = 0;

		for (const auto &datagram : from.takeOutgoing())
		{
			auto packet = decodePacket(datagram);
			EXPECT_TRUE(packet.has_value()) << "Links must only produce well formed packets";
			if (!packet || (drop && drop(packet->header)))
				continue;

			to.onPacket(*packet, now);
			++arrived;
		}

		collect();
		return arrived;
	}

	void collect()
	{
		for (auto &p : a.takeDelivered())
			atA.push_back(std::move(p));
		for (auto &p : b.takeDelivered())
			atB.push_back(std::move(p));
	}

	// Exchanges packets until nothing moves anymore
	void settle(const Drop &dropAtoB = {}, const Drop &dropBtoA = {})
	{
		for (int round = 0; round < 1000; ++round)
		{
			if (transfer(a, b, dropAtoB) + transfer(b, a, dropBtoA) == 0 && a.takeOutgoing().empty() && b.takeOutgoing().empty())
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

	// Stress tests lose so much that the default retry budget would legitimately give up
	void						recreateWithRetryBudget(int maxRetransmits)
	{
		config.maxRetransmits = maxRetransmits;
		a					  = ReliableLink(config, 0xAAAA0001);
		b					  = ReliableLink(config, 0xBBBB0001);
	}

	static Drop kind(PacketKind packetKind)
	{
		return [packetKind](const PacketHeader &header) { return header.flags.kind() == packetKind; };
	}
};


TEST_F(ReliableLinkTest, HappyPath_DataAckAckAck)
{
	ASSERT_EQ(a.queueReliable(ChannelId::Application, bytes({1, 2, 3}), now), PushResult::Accepted);
	EXPECT_EQ(a.inFlightCount(), 1u);

	EXPECT_EQ(transfer(a, b), 1u) << "One Data packet";
	ASSERT_EQ(atB.size(), 1u);
	EXPECT_EQ(atB[0].body, bytes({1, 2, 3}));
	EXPECT_EQ(atB[0].header.flags.channel(), ChannelId::Application);

	EXPECT_EQ(transfer(b, a), 1u) << "One DataAck";
	EXPECT_EQ(a.inFlightCount(), 0u);
	EXPECT_FALSE(a.hasPendingReliable());

	EXPECT_EQ(transfer(a, b), 1u) << "One AckAck";

	EXPECT_EQ(a.stats().dataSent, 1u);
	EXPECT_EQ(b.stats().dataAcksSent, 1u);
	EXPECT_EQ(a.stats().ackAcksSent, 1u);

	// Nothing is left to retransmit on either side
	advance(5s);
	EXPECT_TRUE(a.takeOutgoing().empty());
	EXPECT_TRUE(b.takeOutgoing().empty());
	EXPECT_EQ(a.stats().retransmissions, 0u);
}


TEST_F(ReliableLinkTest, LostData_IsRetransmittedAfterTheRto)
{
	a.queueReliable(ChannelId::Application, bytes({7}), now);
	a.takeOutgoing(); // lost on the wire

	advance(99ms);
	EXPECT_TRUE(a.takeOutgoing().empty()) << "Not before the RTO";

	advance(1ms);
	settle();

	ASSERT_EQ(atB.size(), 1u);
	EXPECT_EQ(atB[0].body, bytes({7}));
	EXPECT_EQ(a.stats().retransmissions, 1u);
	EXPECT_FALSE(a.hasPendingReliable());
}


TEST_F(ReliableLinkTest, LostDataAck_DataIsResentButDeliveredOnce)
{
	a.queueReliable(ChannelId::Application, bytes({7}), now);
	transfer(a, b);
	b.takeOutgoing(); // the DataAck is lost
	ASSERT_EQ(atB.size(), 1u);

	advance(100ms);
	settle();

	EXPECT_EQ(atB.size(), 1u) << "A retransmitted Data packet must not be delivered twice";
	EXPECT_GE(b.stats().duplicatesReceived, 1u);
	EXPECT_FALSE(a.hasPendingReliable()) << "The re-sent DataAck completes the message";
}


TEST_F(ReliableLinkTest, LostAckAck_DataAckIsResentAndConfirmedAgain)
{
	a.queueReliable(ChannelId::Application, bytes({7}), now);
	transfer(a, b);
	transfer(b, a);
	a.takeOutgoing(); // the AckAck is lost
	EXPECT_EQ(b.stats().dataAcksSent, 1u);

	advance(100ms);
	EXPECT_EQ(b.stats().dataAcksSent, 2u) << "Without AckAck the receiver resends its DataAck";

	settle();
	EXPECT_EQ(a.stats().ackAcksSent, 2u) << "An already completed key is confirmed again";

	// The AckAck arrived: the receiver stops resending
	advance(1s);
	advance(1s);
	EXPECT_EQ(b.stats().dataAcksSent, 2u);
	EXPECT_EQ(atB.size(), 1u);
}


TEST_F(ReliableLinkTest, ReceiverStopsResendingDataAckAfterItsLimit)
{
	a.queueReliable(ChannelId::Application, bytes({7}), now);
	transfer(a, b);
	transfer(b, a);
	a.takeOutgoing(); // every AckAck gets lost from here on

	for (int i = 0; i < 20; ++i)
	{
		advance(500ms);
		b.takeOutgoing();
	}

	EXPECT_EQ(b.stats().dataAcksSent, 1u + static_cast<uint64_t>(config.maxAckRetransmits)) << "DataAck resends are bounded";
}


TEST_F(ReliableLinkTest, ReorderedData_IsDeliveredInOrder)
{
	a.queueReliable(ChannelId::Application, bytes({1}), now);
	a.queueReliable(ChannelId::Application, bytes({2}), now);
	a.queueReliable(ChannelId::Application, bytes({3}), now);

	auto datagrams = a.takeOutgoing();
	ASSERT_EQ(datagrams.size(), 3u);

	for (size_t index : {2u, 0u, 1u})
		b.onPacket(*decodePacket(datagrams[index]), now);
	collect();

	ASSERT_EQ(atB.size(), 3u);
	EXPECT_EQ(atB[0].body, bytes({1}));
	EXPECT_EQ(atB[1].body, bytes({2}));
	EXPECT_EQ(atB[2].body, bytes({3}));
	EXPECT_EQ(b.stats().dataAcksSent, 3u) << "Each message is acknowledged individually, also when buffered";
}


TEST_F(ReliableLinkTest, DuplicatedDatagram_IsDeliveredOnce)
{
	a.queueReliable(ChannelId::Application, bytes({1}), now);
	auto datagrams = a.takeOutgoing();

	b.onPacket(*decodePacket(datagrams[0]), now);
	b.onPacket(*decodePacket(datagrams[0]), now);
	collect();

	EXPECT_EQ(atB.size(), 1u);
	EXPECT_EQ(b.stats().duplicatesReceived, 1u);
	EXPECT_EQ(b.stats().dataAcksSent, 2u) << "The duplicate is acknowledged too, its sender may have missed the first DataAck";
}


TEST_F(ReliableLinkTest, ExhaustedRetransmissions_FailTheLinkAndStartANewStream)
{
	const uint32_t streamID = a.localStreamID();
	a.queueReliable(ChannelId::Application, bytes({1}), now);

	bool failed = false;
	for (int i = 0; i < 30 && !failed; ++i)
	{
		advance(400ms);
		a.takeOutgoing(); // everything is lost

		for (auto event : a.takeEvents())
			failed |= event == LinkEvent::Failed;
	}

	ASSERT_TRUE(failed);
	EXPECT_EQ(a.stats().retransmissions, static_cast<uint64_t>(config.maxRetransmits));
	EXPECT_FALSE(a.hasPendingReliable()) << "The failed stream is discarded";
	EXPECT_NE(a.localStreamID(), streamID) << "A new stream needs a new stream ID";
}


TEST_F(ReliableLinkTest, AfterFailure_BothSidesResynchronise)
{
	// Establish both streams first
	a.queueReliable(ChannelId::Application, bytes({1}), now);
	b.queueReliable(ChannelId::Application, bytes({2}), now);
	settle();
	ASSERT_EQ(atB.size(), 1u);
	ASSERT_EQ(atA.size(), 1u);

	// A's next message never gets through: A fails
	a.queueReliable(ChannelId::Application, bytes({3}), now);
	bool failed = false;
	for (int i = 0; i < 30 && !failed; ++i)
	{
		advance(400ms);
		a.takeOutgoing();
		b.takeOutgoing();
		for (auto event : a.takeEvents())
			failed |= event == LinkEvent::Failed;
	}
	ASSERT_TRUE(failed);

	// The network recovers
	a.queueReliable(ChannelId::Application, bytes({4}), now);
	settle();

	ASSERT_EQ(atB.size(), 2u);
	EXPECT_EQ(atB[1].body, bytes({4}));

	auto eventsB = b.takeEvents();
	ASSERT_EQ(eventsB.size(), 1u);
	EXPECT_EQ(eventsB[0], LinkEvent::PeerRestarted) << "The new stream ID tells B to reset as well";

	b.queueReliable(ChannelId::Application, bytes({5}), now);
	settle();
	ASSERT_EQ(atA.size(), 2u);
	EXPECT_EQ(atA[1].body, bytes({5})) << "B's stream to A continues after the reset";
}


TEST_F(ReliableLinkTest, PeerRestart_ResetsTheLink)
{
	a.queueReliable(ChannelId::Application, bytes({1}), now);
	settle();
	ASSERT_EQ(a.remoteStreamID(), b.localStreamID());

	// B restarts: a brand-new link with a new stream ID starts at seq 1 again
	ReliableLink restarted(config, 0xBBBB0002);
	a.queueReliable(ChannelId::Application, bytes({2}), now); // queued towards the old B
	a.takeOutgoing();

	restarted.queueReliable(ChannelId::Control, bytes({9}), now);
	for (const auto &datagram : restarted.takeOutgoing())
		a.onPacket(*decodePacket(datagram), now);
	collect();

	auto events = a.takeEvents();
	ASSERT_EQ(events.size(), 1u);
	EXPECT_EQ(events[0], LinkEvent::PeerRestarted);
	EXPECT_EQ(a.remoteStreamID(), 0xBBBB0002u);
	EXPECT_FALSE(a.hasPendingReliable()) << "Messages for the old stream are dropped";

	ASSERT_EQ(atA.size(), 1u) << "seq 1 of the new stream is not mistaken for a duplicate";
	EXPECT_EQ(atA[0].body, bytes({9}));
}


TEST_F(ReliableLinkTest, PacketForAnOldStream_IsDropped)
{
	a.queueReliable(ChannelId::Application, bytes({1}), now);
	settle();

	PacketHeader header;
	header.flags	   = PacketFlags::data(ChannelId::Application, true);
	header.srcStreamID = a.localStreamID();
	header.dstStreamID = 0x12345678; // not B
	header.seq		   = 2;

	b.onPacket(*decodePacket(encodePacket(header, bytes({5}))), now);
	collect();

	EXPECT_EQ(b.stats().staleDropped, 1u);
	EXPECT_EQ(atB.size(), 1u);
	EXPECT_TRUE(b.takeOutgoing().empty()) << "A stale packet is not acknowledged";
}


TEST_F(ReliableLinkTest, DataBeyondTheReceiveWindow_IsNotAcknowledged)
{
	PacketHeader header;
	header.flags	   = PacketFlags::data(ChannelId::Application, true);
	header.srcStreamID = a.localStreamID();
	header.seq		   = 1 + WindowSize;

	b.onPacket(*decodePacket(encodePacket(header, bytes({5}))), now);

	EXPECT_EQ(b.stats().outOfWindowDropped, 1u);
	EXPECT_TRUE(b.takeOutgoing().empty());
}


TEST_F(ReliableLinkTest, SendWindowLimitsPacketsInFlight)
{
	const size_t total = WindowSize + 44;
	for (size_t i = 0; i < total; ++i)
		ASSERT_EQ(a.queueReliable(ChannelId::Application, {static_cast<uint8_t>(i), static_cast<uint8_t>(i >> 8)}, now), PushResult::Accepted);

	EXPECT_EQ(a.inFlightCount(), WindowSize);
	EXPECT_EQ(a.queuedMessageCount(), 44u);

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

	for (size_t i = 0; i < WindowSize; ++i)
		link.queueReliable(ChannelId::Application, bytes({1}), now);

	EXPECT_EQ(link.queueReliable(ChannelId::Application, bytes({2}), now), PushResult::Accepted);
	EXPECT_EQ(link.queueReliable(ChannelId::Application, bytes({3}), now), PushResult::Accepted);
	EXPECT_EQ(link.queueReliable(ChannelId::Application, bytes({4}), now), PushResult::Rejected);
	EXPECT_EQ(link.queueReliable(ChannelId::Control, bytes({5}), now), PushResult::Accepted) << "Control signals never compete with application backpressure";
}


TEST_F(ReliableLinkTest, DropOldest_EvictsUnsentMessagesWithoutLeavingAGap)
{
	config.sendQueueCapacity = 4;
	config.sendQueueOverflow = OverflowPolicy::DropOldest;
	ReliableLink sender(config, 0xAAAA0009);

	for (size_t i = 0; i < WindowSize; ++i)
		sender.queueReliable(ChannelId::Application, bytes({0}), now);

	for (uint8_t value = 1; value <= 6; ++value)
	{
		const auto result = sender.queueReliable(ChannelId::Application, bytes({value}), now);
		EXPECT_EQ(result, value <= 4 ? PushResult::Accepted : PushResult::EvictedOldest);
	}

	// Run the exchange to completion
	for (int round = 0; round < 1000; ++round)
	{
		size_t moved = 0;
		for (const auto &datagram : sender.takeOutgoing())
		{
			b.onPacket(*decodePacket(datagram), now);
			++moved;
		}
		for (const auto &datagram : b.takeOutgoing())
		{
			sender.onPacket(*decodePacket(datagram), now);
			++moved;
		}
		if (moved == 0)
			break;
	}
	collect();

	ASSERT_EQ(atB.size(), WindowSize + 4) << "Every message that was kept arrives: evicting never created a gap in the stream";
	EXPECT_EQ(atB[WindowSize].body, bytes({3})) << "1 and 2 were the oldest unsent messages";
	EXPECT_EQ(atB.back().body, bytes({6}));
	EXPECT_FALSE(sender.hasPendingReliable());
}


TEST_F(ReliableLinkTest, ControlSignalsOvertakeQueuedApplicationMessages)
{
	for (size_t i = 0; i < WindowSize + 5; ++i)
		a.queueReliable(ChannelId::Application, bytes({1}), now);

	a.queueReliable(ChannelId::Control, bytes({42}), now);
	settle();

	ASSERT_EQ(atB.size(), WindowSize + 6);
	EXPECT_EQ(atB[WindowSize].header.flags.channel(), ChannelId::Control) << "The first free window slot goes to the control signal";
}


TEST_F(ReliableLinkTest, LargeMessageIsFragmentedAndReassembledUnderLoss)
{
	std::vector<uint8_t> body(100 * 1024);
	for (size_t i = 0; i < body.size(); ++i)
		body[i] = static_cast<uint8_t>(i * 13);

	recreateWithRetryBudget(20);
	ASSERT_EQ(a.queueReliable(ChannelId::Application, body, now), PushResult::Accepted);

	std::mt19937 random(3);
	auto		 lossy = [&random](const PacketHeader &) { return std::uniform_real_distribution<double>(0.0, 1.0)(random) < 0.2; };

	for (int i = 0; i < 400 && a.hasPendingReliable(); ++i)
	{
		transfer(a, b, lossy);
		transfer(b, a, lossy);
		advance(50ms);
	}
	settle();

	ASSERT_FALSE(a.hasPendingReliable());

	FragmentationService			  reassembly;
	std::optional<ReassembledMessage> message;
	for (const auto &packet : atB)
	{
		EXPECT_TRUE(packet.header.flags.isFragmented());
		if (auto m = reassembly.accept({ipv4("10.0.0.1"), 1}, packet.header, packet.body))
			message = std::move(m);
	}

	ASSERT_TRUE(message.has_value());
	EXPECT_EQ(message->body, body);
	EXPECT_GT(a.stats().retransmissions, 0u) << "The loss must actually have been exercised";
}


TEST_F(ReliableLinkTest, ManyMessagesUnderLossDuplicationAndReordering)
{
	recreateWithRetryBudget(40);

	std::mt19937 random(11);
	auto		 roll	= [&random](double p) { return std::uniform_real_distribution<double>(0.0, 1.0)(random) < p; };

	const int	 total	= 2000;
	int			 queued = 0;

	for (int step = 0; step < 5000 && (queued < total || a.hasPendingReliable()); ++step)
	{
		while (queued < total && a.queuedMessageCount() < 64)
		{
			a.queueReliable(ChannelId::Application, {static_cast<uint8_t>(queued), static_cast<uint8_t>(queued >> 8)}, now);
			++queued;
		}

		// A misbehaving network in both directions
		for (auto *pair : {&a, &b})
		{
			ReliableLink &from		= *pair;
			ReliableLink &to		= pair == &a ? b : a;
			auto		  datagrams = from.takeOutgoing();
			std::shuffle(datagrams.begin(), datagrams.end(), random);

			for (const auto &datagram : datagrams)
			{
				if (roll(0.25))
					continue;
				to.onPacket(*decodePacket(datagram), now);
				if (roll(0.1))
					to.onPacket(*decodePacket(datagram), now);
			}
		}

		collect();
		advance(10ms);
	}

	ASSERT_EQ(atB.size(), static_cast<size_t>(total)) << "Every message exactly once";
	for (int i = 0; i < total; ++i)
		ASSERT_EQ(atB[i].body, (std::vector<uint8_t>{static_cast<uint8_t>(i), static_cast<uint8_t>(i >> 8)})) << "in order at " << i;
}


TEST_F(ReliableLinkTest, Unreliable_StaleMessagesAreDiscarded)
{
	ASSERT_TRUE(a.sendUnreliable(ChannelId::Application, bytes({1})));
	ASSERT_TRUE(a.sendUnreliable(ChannelId::Application, bytes({2})));
	ASSERT_TRUE(a.sendUnreliable(ChannelId::Application, bytes({3})));

	auto datagrams = a.takeOutgoing();
	ASSERT_EQ(datagrams.size(), 3u);

	b.onPacket(*decodePacket(datagrams[1]), now);
	b.onPacket(*decodePacket(datagrams[0]), now); // older than what was delivered
	b.onPacket(*decodePacket(datagrams[2]), now);
	collect();

	ASSERT_EQ(atB.size(), 2u);
	EXPECT_EQ(atB[0].body, bytes({2}));
	EXPECT_EQ(atB[1].body, bytes({3}));
	EXPECT_TRUE(b.takeOutgoing().empty()) << "Unreliable messages are never acknowledged";
	EXPECT_FALSE(a.hasPendingReliable());
}


TEST_F(ReliableLinkTest, Unreliable_MustFitIntoOneDatagram)
{
	std::vector<uint8_t> fits(a.maxUnreliableBody());
	std::vector<uint8_t> tooLarge(a.maxUnreliableBody() + 1);

	EXPECT_TRUE(a.sendUnreliable(ChannelId::Application, fits));
	EXPECT_FALSE(a.sendUnreliable(ChannelId::Application, tooLarge));

	for (const auto &datagram : a.takeOutgoing())
		EXPECT_LE(datagram.size(), config.maxDatagramSize);
}


TEST_F(ReliableLinkTest, NoDatagramExceedsTheMaximumSize)
{
	a.queueReliable(ChannelId::Application, std::vector<uint8_t>(10000, 1), now);

	for (const auto &datagram : a.takeOutgoing())
		EXPECT_LE(datagram.size(), config.maxDatagramSize);
}


TEST_F(ReliableLinkTest, OversizedMessage_IsRejected)
{
	config.maxMessageSize = 1000;
	ReliableLink link(config);

	EXPECT_EQ(link.queueReliable(ChannelId::Application, std::vector<uint8_t>(1001), now), PushResult::Rejected);
	EXPECT_FALSE(link.hasPendingReliable());
}


TEST_F(ReliableLinkTest, DropQueuedApplicationMessages_KeepsControlSignals)
{
	for (size_t i = 0; i < WindowSize + 3; ++i)
		a.queueReliable(ChannelId::Application, bytes({1}), now);
	a.queueReliable(ChannelId::Control, bytes({2}), now);

	a.dropQueuedApplicationMessages();
	EXPECT_EQ(a.queuedMessageCount(), 1u) << "Only the control signal is left in the queue";

	settle();
	EXPECT_EQ(atB.size(), WindowSize + 1) << "Messages already in flight still complete";
}


TEST_F(ReliableLinkTest, RttIsSampledFromUnretransmittedPackets)
{
	a.queueReliable(ChannelId::Application, bytes({1}), now);
	transfer(a, b);
	now += 30ms;
	transfer(b, a);

	ASSERT_TRUE(a.rtt().hasSample());
	EXPECT_EQ(a.rtt().srtt(), 30ms);
}


TEST_F(ReliableLinkTest, HeartbeatIsNeitherDeliveredNorAcknowledged)
{
	a.sendHeartbeat();
	transfer(a, b);

	EXPECT_TRUE(atB.empty());
	EXPECT_TRUE(b.takeOutgoing().empty());
	EXPECT_EQ(b.remoteStreamID(), a.localStreamID()) << "A heartbeat still introduces the peer";
}

} // namespace ChannelTests
