#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "TestIp.h"
#include "Channel/PeerChannel.h"
#include "FakeDatagramNetwork.h"
#include "SimDriver.h"

using namespace netlink;
using namespace std::chrono_literals;


namespace SimTests
{

// Peer channels on one fake network, run by a SimDriver: time only passes when the test lets it
struct Scenario
{
	struct Node
	{
		std::string										 name;
		std::string										 ip;
		std::unique_ptr<PeerChannel>					 channel;
		std::vector<std::chrono::microseconds>			 received; // when each message arrived
		std::vector<std::pair<std::string, std::string>> lost;	   // peer and reason
	};

	Node &add(const std::string &name, const std::string &ip, const PeerChannelConfig &config = {})
	{
		Node &node	 = *nodes.emplace_back(std::make_unique<Node>());
		node.name	 = name;
		node.ip		 = ip;
		node.channel = std::make_unique<PeerChannel>(network->factory(ip), config);

		EXPECT_TRUE(node.channel->init(name));
		node.channel->setLocalIPv4(ipv4(ip));

		node.channel->setMessageCallback([this, &node](const std::string &, uint32_t, std::vector<uint8_t>) { node.received.push_back(driver.elapsed()); });
		node.channel->setOnPeerLost([&node](const std::string &peer, const std::string &reason) { node.lost.emplace_back(peer, reason); });

		driver.add(*node.channel);
		return node;
	}

	static void introduce(const Node &a, const Node &b)
	{
		a.channel->registerPeer(b.name, ipv4(b.ip), b.channel->getBoundPort());
		b.channel->registerPeer(a.name, ipv4(a.ip), a.channel->getBoundPort());
	}

	std::shared_ptr<FakeNet::FakeDatagramNetwork> network = FakeNet::FakeDatagramNetwork::create();
	FakeNet::SimDriver							  driver{network};
	std::vector<std::unique_ptr<Node>>			  nodes; // after the driver: the channels go first
};


static std::vector<uint8_t> payload(const size_t size)
{
	return std::vector<uint8_t>(size, 0x5A);
}


TEST(PacingSimTest, DatagramRate_FollowsTheBudget)
{
	for (const uint32_t rate : {1000u, 5000u, 20'000u, 80'000u})
	{
		Scenario		  sim;
		PeerChannelConfig config;
		config.maxSendRate = rate;

		auto &hub		   = sim.add("hub", "10.0.0.1", config);
		auto &peer		   = sim.add("peer", "10.0.0.2");
		Scenario::introduce(hub, peer);

		// More than the budget allows in every millisecond: what does not fit is dropped before it is sent
		const auto flood = [&](const int milliseconds)
		{
			const auto message = payload(64);

			for (int ms = 0; ms < milliseconds; ++ms)
			{
				for (int i = 0; i < 100; ++i)
					hub.channel->sendMessage("peer", 1, message, DeliveryMode::UnreliableSequenced);

				sim.driver.run(1ms);
			}
		};

		flood(100); // the first burst is behind

		const size_t before = sim.network->sentBy(hub.ip);
		flood(1000);
		const auto sent = static_cast<double>(sim.network->sentBy(hub.ip) - before);

		EXPECT_NEAR(sent, rate, rate * 0.02) << "at " << rate << " datagrams per second";
		EXPECT_NEAR(static_cast<double>(peer.received.size()), rate * 1.1, rate * 0.05) << "What was sent arrived";
	}
}


TEST(PacingSimTest, Acknowledgements_CountAgainstTheBudget)
{
	Scenario		  sim;
	PeerChannelConfig config;
	config.maxSendRate = 20'000;

	auto &hub		   = sim.add("hub", "10.0.0.1", config);
	auto &peer		   = sim.add("peer", "10.0.0.2");
	Scenario::introduce(hub, peer);

	// Enough reliable data for the whole measurement: the hub also has to confirm the peer's acknowledgements
	for (int i = 0; i < 3; ++i)
		ASSERT_TRUE(hub.channel->sendMessage("peer", 1, payload(size_t{12} * 1024 * 1024), DeliveryMode::ReliableOrdered));

	sim.driver.run(100ms);

	const size_t before = sim.network->sentBy(hub.ip);
	sim.driver.run(1s);
	const auto sent = static_cast<double>(sim.network->sentBy(hub.ip) - before);

	EXPECT_NEAR(sent, 20'000.0, 20'000.0 * 0.02) << "Data and acknowledgements together stay within the budget";
	EXPECT_TRUE(hub.lost.empty());
}


TEST(PacingSimTest, FanOut_ServesEveryPeerAtTheSameRate)
{
	constexpr int Peers = 16;

	Scenario	  sim;
	auto		 &hub = sim.add("hub", "10.0.0.1");

	for (int i = 0; i < Peers; ++i)
		Scenario::introduce(hub, sim.add("peer-" + std::to_string(i), "10.0.0." + std::to_string(10 + i)));

	for (int i = 0; i < Peers; ++i)
		ASSERT_TRUE(hub.channel->sendMessage("peer-" + std::to_string(i), 1, payload(256 * 1024), DeliveryMode::ReliableOrdered));

	const auto allArrived = [&] { return std::ranges::all_of(sim.nodes, [&](const auto &node) { return node.get() == &hub || !node->received.empty(); }); };
	ASSERT_TRUE(sim.driver.runUntil(allArrived, 5s));

	std::vector<std::chrono::microseconds> finished;
	for (const auto &node : sim.nodes)
	{
		if (node.get() != &hub)
			finished.push_back(node->received.front());
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
	Scenario::introduce(hub, peer);

	// The socket puts a quarter of what the budget allows on the wire, and refuses more than 32 waiting datagrams
	FakeNet::LinkProfile profile;
	profile.bandwidth	  = 20'000 * 1200;
	profile.queueLimit	  = 32;
	profile.blockWhenFull = true;
	sim.network->setProfile(profile);

	// Every reliable Data packet that goes out, and those that go out more than once
	std::set<std::tuple<net::SocketAddress, uint32_t, channel::ChannelId, uint64_t>> seen;
	size_t																			 repeated = 0;

	sim.network->setTap(
		[&](const net::SocketAddress &from, const net::SocketAddress &, const std::span<const uint8_t> data)
		{
			const auto packet = channel::decodePacket(data);
			if (!packet || packet->header.flags.kind() != channel::PacketKind::Data || !packet->header.flags.isReliable())
				return;

			if (!seen.emplace(from, packet->header.srcStreamID, packet->header.flags.channel(), packet->header.seq).second)
				++repeated;
		});

	ASSERT_TRUE(hub.channel->sendMessage("peer", 1, payload(1024 * 1024), DeliveryMode::ReliableOrdered));
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
	Scenario::introduce(hub, peer);

	sim.network->setHostDown(peer.ip, true);
	ASSERT_TRUE(hub.channel->sendMessage("peer", 1, payload(100), DeliveryMode::ReliableOrdered));

	sim.driver.run(300ms);
	EXPECT_TRUE(peer.received.empty());
	EXPECT_TRUE(hub.lost.empty()) << "A send that fails is a lost datagram: the link keeps trying";

	sim.network->setHostDown(peer.ip, false);

	ASSERT_TRUE(sim.driver.runUntil([&] { return !peer.received.empty(); }, 2s)) << "The next retransmission gets through";
	sim.driver.run(1s);
	EXPECT_EQ(peer.received.size(), 1u);

	// A host that stays down is given up on like one that does not answer
	sim.network->setHostDown(peer.ip, true);
	ASSERT_TRUE(hub.channel->sendMessage("peer", 2, payload(100), DeliveryMode::ReliableOrdered));

	const auto since = sim.driver.elapsed();
	ASSERT_TRUE(sim.driver.runUntil([&] { return !hub.lost.empty(); }, 30s));
	EXPECT_GE(sim.driver.elapsed() - since, PeerChannelConfig{}.reliability.failureTimeout);
	EXPECT_EQ(hub.lost.front().first, "peer");
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
		Scenario::introduce(hub, other);
		Scenario::introduce(hub, good);

		sim.network->setHostDown(other.ip, otherIsDown);

		EXPECT_TRUE(hub.channel->sendMessage("other", 1, payload(512 * 1024), DeliveryMode::ReliableOrdered));
		EXPECT_TRUE(hub.channel->sendMessage("good", 1, payload(512 * 1024), DeliveryMode::ReliableOrdered));

		EXPECT_TRUE(sim.driver.runUntil([&] { return !good.received.empty(); }, 5s));
		return good.received.empty() ? 5s : good.received.front();
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
	Scenario::introduce(hub, a);
	Scenario::introduce(hub, b);

	ASSERT_TRUE(hub.channel->sendMessage("a", 1, payload(100), DeliveryMode::ReliableOrdered));
	ASSERT_TRUE(hub.channel->sendMessage("b", 1, payload(100), DeliveryMode::ReliableOrdered));
	ASSERT_TRUE(sim.driver.runUntil([&] { return !a.received.empty() && !b.received.empty(); }, 1s));

	sim.network->setSendError(hub.ip, net::SocketError::NetworkDown);
	ASSERT_TRUE(hub.channel->sendMessage("a", 2, payload(100), DeliveryMode::ReliableOrdered));
	sim.driver.run(10ms);

	ASSERT_EQ(hub.lost.size(), 2u) << "A socket that cannot send anymore ends every session, not only the one that noticed";
	EXPECT_NE(hub.lost[0].first, hub.lost[1].first);
	EXPECT_TRUE(hub.channel->flush("a", 0ms)) << "Nothing is left waiting for an acknowledgement that cannot come";
}

} // namespace SimTests
