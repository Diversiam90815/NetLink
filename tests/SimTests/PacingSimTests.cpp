#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "SimScenario.h"
#include "TestIp.h"

using namespace netlink;
using namespace std::chrono_literals;


namespace SimTests
{

using Scenario = FakeNet::SimScenario;


static std::vector<uint8_t> payload(const size_t size)
{
	return std::vector<uint8_t>(size, 0x5A);
}


TEST(PacingSimTest, DatagramRate_FollowsTheBudget)
{
	constexpr size_t DatagramsPerMessage = 4;

	for (const uint32_t rate : {1000u, 5000u, 20'000u, 80'000u})
	{
		Scenario	 sim;
		EngineConfig config = Scenario::defaultConfig();
		config.maxSendRate	= rate;

		auto &hub			= sim.add("hub", "10.0.0.1", config);
		auto &peer			= sim.add("peer", "10.0.0.2");
		ASSERT_TRUE(sim.connect(hub, peer));

		// More than the budget allows in every millisecond: what does not fit is dropped before it is sent
		const auto flood = [&](const int milliseconds)
		{
			const auto message = payload(DatagramsPerMessage * channel::MaxFragmentBody);

			for (int ms = 0; ms < milliseconds; ++ms)
			{
				for (int i = 0; i < 30; ++i)
					hub.engine->send(peer.id(), 1, std::vector<uint8_t>(message), Lane::Media);

				sim.driver.run(1ms);
			}
		};

		flood(100); // the first burst is behind

		const size_t before = sim.network->sentBy(hub.ip);
		flood(1000);
		const auto sent = static_cast<double>(sim.network->sentBy(hub.ip) - before);

		EXPECT_NEAR(sent, rate, rate * 0.02) << "at " << rate << " datagrams per second";
		EXPECT_NEAR(static_cast<double>(peer.received.size() * DatagramsPerMessage), rate * 1.1, rate * 0.05) << "What was sent arrived";
	}
}


TEST(PacingSimTest, Acknowledgements_CountAgainstTheBudget)
{
	Scenario	 sim;
	EngineConfig config = Scenario::defaultConfig();
	config.maxSendRate	= 20'000;

	auto &hub			= sim.add("hub", "10.0.0.1", config);
	auto &peer			= sim.add("peer", "10.0.0.2");
	ASSERT_TRUE(sim.connect(hub, peer));

	// Enough reliable data for the whole measurement, in both directions: the hub also acknowledges what the peer sends
	for (int i = 0; i < 3; ++i)
	{
		ASSERT_EQ(hub.engine->send(peer.id(), 1, payload(size_t{12} * 1024 * 1024), Lane::Reliable), SendResult::Queued);
		ASSERT_EQ(peer.engine->send(hub.id(), 1, payload(size_t{12} * 1024 * 1024), Lane::Reliable), SendResult::Queued);
	}

	sim.driver.run(100ms);

	const size_t before = sim.network->sentBy(hub.ip);
	sim.driver.run(1s);
	const auto sent = static_cast<double>(sim.network->sentBy(hub.ip) - before);

	EXPECT_NEAR(sent, 20'000.0, 20'000.0 * 0.02) << "Data and acknowledgements together stay within the budget";
	EXPECT_TRUE(hub.events.ended().empty());
}


TEST(PacingSimTest, FanOut_ServesEveryPeerAtTheSameRate)
{
	constexpr int Peers = 16;

	Scenario	  sim;
	auto		 &hub = sim.add("hub", "10.0.0.1");

	for (int i = 0; i < Peers; ++i)
		ASSERT_TRUE(sim.connect(hub, sim.add("peer-" + std::to_string(i), "10.0.0." + std::to_string(10 + i))));

	// The same message for all of them, at the same moment
	const auto started = sim.driver.elapsed();
	ASSERT_EQ(hub.engine->broadcast(1, payload(256 * 1024), Lane::Reliable), static_cast<size_t>(Peers));

	const auto allArrived = [&] { return std::ranges::all_of(sim.nodes, [&](const auto &node) { return node.get() == &hub || !node->received.empty(); }); };
	ASSERT_TRUE(sim.driver.runUntil(allArrived, 5s));

	std::vector<std::chrono::microseconds> finished;
	for (const auto &node : sim.nodes)
	{
		if (node.get() != &hub)
			finished.push_back(node->received.front() - started);
	}

	const auto [first, last] = std::ranges::minmax(finished);
	EXPECT_LE(static_cast<double>((last - first).count()), 0.10 * static_cast<double>(last.count()))
		<< "The first peer was done after " << first.count() << " us, the last one after " << last.count() << " us";
}


TEST(PacingSimTest, WouldBlock_CausesNoRetransmissions)
{
	Scenario sim;
	auto	&hub  = sim.add("hub", "10.0.0.1");
	auto	&peer = sim.add("peer", "10.0.0.2");
	ASSERT_TRUE(sim.connect(hub, peer));

	// The socket puts a quarter of what the budget allows on the wire, and refuses more than 32 waiting datagrams
	FakeNet::LinkProfile profile;
	profile.bandwidth	  = 20'000 * 1200;
	profile.queueLimit	  = 32;
	profile.blockWhenFull = true;
	sim.network->setProfile(profile);

	// Every reliable Data packet that goes out, and those that go out more than once
	std::set<std::tuple<net::SocketAddress, uint32_t, channel::Lane, uint64_t>> seen;
	size_t																		repeated = 0;

	sim.network->setTap(
		[&](const net::SocketAddress &from, const net::SocketAddress &, const std::span<const uint8_t> data)
		{
			const auto packet = channel::decodePacket(data);
			if (!packet || packet->header.flags.kind() != channel::PacketKind::Data || packet->header.flags.lane() == channel::Lane::Media)
				return;

			if (!seen.emplace(from, packet->header.srcStreamID, packet->header.flags.lane(), packet->header.seq).second)
				++repeated;
		});

	ASSERT_EQ(hub.engine->send(peer.id(), 1, payload(1024 * 1024), Lane::Reliable), SendResult::Queued);
	ASSERT_TRUE(sim.driver.runUntil([&] { return !peer.received.empty(); }, 10s));

	sim.network->setTap({});

	EXPECT_GT(sim.network->blockedSends(), 100u) << "The socket must really have refused datagrams";
	EXPECT_EQ(repeated, 0u) << "A datagram the socket did not take was never sent: there is nothing to send again";
	EXPECT_GT(seen.size(), 800u);
}


TEST(PacingSimTest, HostDown_IsTreatedAsLoss)
{
	Scenario sim;
	auto	&hub  = sim.add("hub", "10.0.0.1");
	auto	&peer = sim.add("peer", "10.0.0.2");
	ASSERT_TRUE(sim.connect(hub, peer));

	sim.network->setHostDown(peer.ip, true);
	ASSERT_EQ(hub.engine->send(peer.id(), 1, payload(100), Lane::Reliable), SendResult::Queued);

	sim.driver.run(300ms);
	EXPECT_TRUE(peer.received.empty());
	EXPECT_TRUE(hub.events.ended().empty()) << "A send that fails is a lost datagram: the link keeps trying";

	sim.network->setHostDown(peer.ip, false);

	ASSERT_TRUE(sim.driver.runUntil([&] { return !peer.received.empty(); }, 2s)) << "The next retransmission gets through";
	sim.driver.run(1s);
	EXPECT_EQ(peer.received.size(), 1u);

	// A host that stays down is given up on like one that does not answer
	sim.network->setHostDown(peer.ip, true);
	ASSERT_EQ(hub.engine->send(peer.id(), 2, payload(100), Lane::Reliable), SendResult::Queued);

	const auto since   = sim.driver.elapsed();
	const auto timings = Scenario::defaultConfig().timings;

	ASSERT_TRUE(sim.driver.runUntil([&] { return !hub.events.ended().empty(); }, 30s));
	EXPECT_GE(sim.driver.elapsed() - since, timings.peerTimeout - timings.keepAlive) << "Not before the peer was silent for that long";
	EXPECT_EQ(hub.events.ended().front(), (FakeNet::Ended{peer.id(), DisconnectReason::Lost}));
}


TEST(PacingSimTest, UnreachablePeer_DoesNotStallOthers)
{
	// How long 512 KiB take to reach a healthy peer while the hub sends as much to another one
	const auto timeToDeliver = [](const bool otherIsDown)
	{
		Scenario sim;
		auto	&hub   = sim.add("hub", "10.0.0.1");
		auto	&other = sim.add("other", "10.0.0.2");
		auto	&good  = sim.add("good", "10.0.0.3");
		EXPECT_TRUE(sim.connect(hub, other));
		EXPECT_TRUE(sim.connect(hub, good));

		sim.network->setHostDown(other.ip, otherIsDown);

		const auto started = sim.driver.elapsed();
		EXPECT_EQ(hub.engine->send(other.id(), 1, payload(512 * 1024), Lane::Reliable), SendResult::Queued);
		EXPECT_EQ(hub.engine->send(good.id(), 1, payload(512 * 1024), Lane::Reliable), SendResult::Queued);

		EXPECT_TRUE(sim.driver.runUntil([&] { return !good.received.empty(); }, 5s));
		return good.received.empty() ? 5s : good.received.front() - started;
	};

	const auto bothHealthy = timeToDeliver(false);
	const auto otherDown   = timeToDeliver(true);

	EXPECT_LE(otherDown, bothHealthy) << "Sends that fail right away must not cost the healthy peer anything";
}


TEST(PacingSimTest, NetworkDown_EndsEverySession)
{
	Scenario sim;
	auto	&hub = sim.add("hub", "10.0.0.1");
	auto	&a	 = sim.add("a", "10.0.0.2");
	auto	&b	 = sim.add("b", "10.0.0.3");
	ASSERT_TRUE(sim.connect(hub, a));
	ASSERT_TRUE(sim.connect(hub, b));

	ASSERT_EQ(hub.engine->send(a.id(), 1, payload(100), Lane::Reliable), SendResult::Queued);
	ASSERT_EQ(hub.engine->send(b.id(), 1, payload(100), Lane::Reliable), SendResult::Queued);
	ASSERT_TRUE(sim.driver.runUntil([&] { return !a.received.empty() && !b.received.empty(); }, 1s));

	sim.network->setSendError(hub.ip, net::SocketError::NetworkDown);
	ASSERT_EQ(hub.engine->send(a.id(), 2, payload(100), Lane::Reliable), SendResult::Queued);
	sim.driver.run(10ms);

	const auto ended = hub.events.ended();
	ASSERT_EQ(ended.size(), 2u) << "A socket that cannot send anymore ends every session, not only the one that noticed";
	EXPECT_NE(ended[0].peer, ended[1].peer);
	EXPECT_EQ(ended[0].reason, DisconnectReason::NetworkError);
	EXPECT_TRUE(hub.engine->flush(a.id(), 0ms)) << "Nothing is left waiting for an acknowledgement that cannot come";
	EXPECT_EQ(hub.engine->send(a.id(), 3, payload(100), Lane::Reliable), SendResult::NotConnected);
}

} // namespace SimTests
