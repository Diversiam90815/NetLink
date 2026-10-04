#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <string>
#include <vector>

#include "Discovery/Beacon.h"
#include "Session/ControlMessage.h"
#include "SimScenario.h"
#include "TestIp.h"

using namespace netlink;
using namespace std::chrono_literals;


namespace SimTests
{

using Scenario = FakeNet::SimScenario;
using Kind	   = EngineEvent::Kind;
using FakeNet::Ended;


static std::vector<uint8_t> payload(const size_t size, const uint8_t fill = 0x5A)
{
	return std::vector<uint8_t>(size, fill);
}

static EngineConfig withId(const uint64_t id, const bool autoAccept = true)
{
	EngineConfig config = Scenario::defaultConfig();
	config.instanceId	= id;
	config.autoAccept	= autoAccept;
	return config;
}

// What a datagram is, for the tests that watch the wire
struct Seen
{
	bool				isBeacon{false};
	channel::PacketKind kind{channel::PacketKind::Data};
	channel::Lane		lane{channel::Lane::Control};
	uint32_t			tag{0};
	bool				startsMessage{false};
};

static Seen look(const std::span<const uint8_t> data)
{
	if (discovery::isBeacon(data))
		return {.isBeacon = true, .kind = channel::PacketKind::Beacon};

	const auto packet = channel::decodePacket(data);
	if (!packet)
		return {};

	return {
		.isBeacon = false, .kind = packet->header.flags.kind(), .lane = packet->header.flags.lane(), .tag = packet->header.tag, .startsMessage = packet->header.startsMessage()};
}

static bool isHello(const Seen &seen)
{
	return seen.kind == channel::PacketKind::Data && seen.lane == channel::Lane::Control && seen.startsMessage && seen.tag == std::to_underlying(session::ControlType::Hello);
}


// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

TEST(SessionSimTest, Peers_AreDiscoveredOnceAndForgottenWhenTheyStopAnnouncing)
{
	Scenario sim;
	auto	&a = sim.add("a", "10.0.0.1");
	auto	&b = sim.add("b", "10.0.0.2");

	ASSERT_TRUE(sim.discover(a, b));
	sim.driver.run(20s);
	EXPECT_EQ(a.events.discovered().size(), 1u) << "Announcements of a peer that is known are not reported again";
	EXPECT_TRUE(a.events.lost().empty());

	sim.freeze(b);
	const auto since = sim.driver.elapsed();

	ASSERT_TRUE(sim.driver.runUntil([&] { return !a.events.lost().empty(); }, 20s));
	EXPECT_EQ(a.events.lost(), std::vector<PeerId>{b.id()});
	EXPECT_GE(sim.driver.elapsed() - since, 4s) << "Three announcements may get lost before a peer is given up on";
	EXPECT_LE(sim.driver.elapsed() - since, 9s);
	EXPECT_TRUE(a.engine->peers().empty());
	EXPECT_FALSE(a.engine->connect(b.id()));
}


TEST(SessionSimTest, OtherApplicationsVersionsAndSubnets_AreNotDiscovered)
{
	Scenario	 sim;
	EngineConfig otherApp	  = Scenario::defaultConfig();
	EngineConfig otherVersion = Scenario::defaultConfig();
	EngineConfig patched	  = Scenario::defaultConfig();
	otherApp.appId			  = "another-app";
	otherVersion.appVersion	  = "2.0";
	patched.appVersion		  = "1.0.7.1234";

	auto &a					  = sim.add("a", "10.0.0.1");
	sim.add("other-app", "10.0.0.2", otherApp);
	sim.add("other-version", "10.0.0.3", otherVersion);
	auto						   &sameRelease = sim.add("patched", "10.0.0.4", patched);

	// Reaches a by limited broadcast, but from outside its subnet
	std::vector<FakeNet::Interface> wide{{.ip = ipv4("10.0.1.9"), .mask = ipv4("255.255.0.0")}};
	auto						   &outsider = sim.add("outsider", "10.0.1.9", sim.network->factory(wide));
	outsider.iface.set("10.0.1.9", "255.255.0.0");

	sim.driver.run(10s);

	const auto discovered = a.events.discovered();
	ASSERT_EQ(discovered.size(), 1u) << "Only the peer of the same application and release";
	EXPECT_EQ(discovered[0].id, sameRelease.id());
	EXPECT_EQ(discovered[0].appVersion, "1.0") << "Patch and build number do not matter";
}


TEST(SessionSimTest, StopAnnouncing_StillFindsOthers)
{
	Scenario sim;
	auto	&quiet = sim.add("quiet", "10.0.0.1");
	auto	&loud  = sim.add("loud", "10.0.0.2");
	quiet.engine->setAnnouncing(false);

	sim.driver.run(10s);

	EXPECT_TRUE(quiet.events.knows(loud.id()));
	EXPECT_FALSE(loud.events.knows(quiet.id())) << "Nothing announced it, not even an answer";
}


TEST(SessionSimTest, LostBeaconsAndHellos_NeverStrandAPeer)
{
	Scenario			 sim;
	auto				&a			 = sim.add("a", "10.0.0.1");
	auto				&b			 = sim.add("b", "10.0.0.2");

	// The first announcements and the first Hellos get lost on the way
	int					 beaconsLost = 0;
	int					 hellosLost	 = 0;

	FakeNet::LinkProfile lossy;
	lossy.lossRate = 0.4;
	lossy.seed	   = 7;
	sim.network->setProfile(lossy);
	sim.network->setTap(
		[&](const net::SocketAddress &, const net::SocketAddress &, const std::span<const uint8_t> data)
		{
			const auto seen = look(data);
			beaconsLost += seen.isBeacon ? 1 : 0;
			hellosLost += isHello(seen) ? 1 : 0;
		});

	ASSERT_TRUE(sim.connect(a, b, 60s)) << "Whatever is lost is sent again: nobody waits for a datagram that will not come";
	EXPECT_GT(beaconsLost, 2);
	EXPECT_GE(hellosLost, 1);

	ASSERT_EQ(a.engine->send(b.id(), 1, payload(10'000), Lane::Reliable), SendResult::Queued);
	EXPECT_TRUE(sim.driver.runUntil([&] { return b.received.size() == 1; }, 30s));
}


TEST(SessionSimTest, SameHostThreeInstances_AllDiscoverAndConnect)
{
	Scenario   sim;

	// Three engines on one machine and one adapter: they share the discovery port
	const auto factory = sim.network->factory("10.0.0.1");
	auto	  &a	   = sim.add("a", "10.0.0.1", factory);
	auto	  &b	   = sim.add("b", "10.0.0.1", factory);
	auto	  &c	   = sim.add("c", "10.0.0.1", factory);

	ASSERT_TRUE(sim.connect(a, b));
	ASSERT_TRUE(sim.connect(b, c));
	ASSERT_TRUE(sim.connect(c, a));

	EXPECT_EQ(a.engine->peers().size(), 2u) << "An engine knows its own announcement by its ID, not by its address";
	EXPECT_EQ(a.engine->connectedPeers().size(), 2u);

	ASSERT_EQ(a.engine->broadcast(1, payload(100), Lane::Reliable), 2u);
	EXPECT_TRUE(sim.driver.runUntil([&] { return b.received.size() == 1 && c.received.size() == 1; }, 5s));
}


TEST(SessionSimTest, MultiHomedHost_DiscoversAndConnects)
{
	Scenario							  sim;

	// One machine in two networks. Its engine runs on the second one.
	const std::vector<FakeNet::Interface> interfaces{{.ip = ipv4("192.168.7.5")}, {.ip = ipv4("10.0.0.1")}};
	auto								 &multi = sim.add("multi", "10.0.0.1", sim.network->factory(interfaces));
	auto								 &peer	= sim.add("peer", "10.0.0.2");
	auto								 &other = sim.add("other", "192.168.7.9");

	ASSERT_TRUE(sim.connect(peer, multi));
	EXPECT_EQ(peer.events.discovered()[0].address, "10.0.0.1") << "A peer is reached at the address its announcement came from";

	sim.driver.run(10s);
	EXPECT_FALSE(multi.events.knows(other.id())) << "The other network is not the one the engine was given";
	EXPECT_FALSE(other.events.knows(multi.id()));

	ASSERT_EQ(multi.engine->send(peer.id(), 1, payload(5000), Lane::Reliable), SendResult::Queued);
	EXPECT_TRUE(sim.driver.runUntil([&] { return peer.received.size() == 1; }, 5s));
}


TEST(SessionSimTest, AdapterIpChange_EndsSessionsRebindsAndRediscovers)
{
	Scenario							  sim;

	const std::vector<FakeNet::Interface> interfaces{{.ip = ipv4("10.0.0.1")}, {.ip = ipv4("10.0.0.50")}};
	auto								 &moving = sim.add("moving", "10.0.0.1", sim.network->factory(interfaces));
	auto								 &peer	 = sim.add("peer", "10.0.0.2");
	ASSERT_TRUE(sim.connect(moving, peer));

	// The adapter gets another address. Nobody tells the engine: it notices with its next look.
	moving.iface.set("10.0.0.50");
	sim.driver.run(3s);

	EXPECT_EQ(moving.events.ended(), std::vector<Ended>({{peer.id(), DisconnectReason::Local}}));
	EXPECT_EQ(moving.events.addresses(), (std::vector<std::string>{"10.0.0.1", "10.0.0.50"}));
	EXPECT_EQ(moving.events.lost(), std::vector<PeerId>{peer.id()}) << "What was found on the old address is looked for again";

	ASSERT_TRUE(
		sim.driver.runUntil([&] { return moving.events.discovered().size() == 2 && !peer.engine->peers().empty() && peer.engine->peers()[0].address == "10.0.0.50"; }, 10s));

	// The peer's session to the old address ends by itself, then a new one can be opened
	ASSERT_TRUE(sim.driver.runUntil([&] { return !peer.events.ended().empty(); }, 10s));
	EXPECT_EQ(peer.events.ended()[0].reason, DisconnectReason::Lost);

	ASSERT_TRUE(peer.engine->connect(moving.id()));
	EXPECT_TRUE(sim.driver.runUntil([&] { return peer.events.connected().size() == 2 && moving.events.connected().size() == 2; }, 5s));
}


TEST(SessionSimTest, AdapterWithoutAddress_EndsSessionsAndComesBack)
{
	Scenario sim;
	auto	&a = sim.add("a", "10.0.0.1");
	auto	&b = sim.add("b", "10.0.0.2");
	ASSERT_TRUE(sim.connect(a, b));

	a.iface.clear();
	sim.driver.run(3s);

	EXPECT_EQ(a.events.ended(), std::vector<Ended>({{b.id(), DisconnectReason::Local}}));
	EXPECT_EQ(a.engine->localEndpoint().port, 0);
	EXPECT_FALSE(a.engine->connect(b.id()));

	a.iface.set("10.0.0.1");
	ASSERT_TRUE(sim.driver.runUntil([&] { return a.events.addresses().size() == 2 && a.events.discovered().size() == 2; }, 10s));
}


// ---------------------------------------------------------------------------
// Opening a session
// ---------------------------------------------------------------------------

TEST(SessionSimTest, Connect_TakesOneRoundTrip)
{
	Scenario sim;
	auto	&a = sim.add("a", "10.0.0.1");
	auto	&b = sim.add("b", "10.0.0.2");
	ASSERT_TRUE(sim.discover(a, b));

	FakeNet::LinkProfile profile;
	profile.latency = 10ms;
	sim.network->setProfile(profile);

	const auto asked = sim.driver.elapsed();
	ASSERT_TRUE(a.engine->connect(b.id()));
	ASSERT_TRUE(sim.driver.runUntil([&] { return a.events.isConnectedTo(b.id()); }, 1s));

	EXPECT_EQ(sim.driver.elapsed() - asked, 20ms) << "The Hello there, the Accept back";

	ASSERT_TRUE(sim.driver.runUntil([&] { return b.events.isConnectedTo(a.id()); }, 1s));
	EXPECT_EQ(sim.driver.elapsed() - asked, 30ms) << "The responder is connected once it knows its Accept arrived";
}


TEST(SessionSimTest, CrossingHellos_BecomeOneSession)
{
	Scenario sim;
	auto	&low  = sim.add("low", "10.0.0.1", withId(100));
	auto	&high = sim.add("high", "10.0.0.2", withId(200));
	ASSERT_TRUE(sim.discover(low, high));

	FakeNet::LinkProfile profile;
	profile.latency = 5ms;
	sim.network->setProfile(profile);

	// Both ask at the same moment: each Hello is on its way when the other one is sent
	ASSERT_TRUE(low.engine->connect(high.id()));
	ASSERT_TRUE(high.engine->connect(low.id()));

	ASSERT_TRUE(sim.driver.runUntil([&] { return low.events.isConnectedTo(high.id()) && high.events.isConnectedTo(low.id()); }, 5s));
	sim.driver.run(10s);

	EXPECT_EQ(low.events.connected().size(), 1u) << "One session, reported once";
	EXPECT_EQ(high.events.connected().size(), 1u);
	EXPECT_TRUE(low.events.ended().empty()) << "The Hello that was given up is no session that ended";
	EXPECT_TRUE(high.events.ended().empty());

	ASSERT_EQ(low.engine->send(high.id(), 1, payload(100), Lane::Reliable), SendResult::Queued);
	ASSERT_EQ(high.engine->send(low.id(), 2, payload(100), Lane::Reliable), SendResult::Queued);
	EXPECT_TRUE(sim.driver.runUntil([&] { return low.received.size() == 1 && high.received.size() == 1; }, 5s));
}


TEST(SessionSimTest, CrossingHellos_AlsoAskNobodyWhoWouldHaveToDecide)
{
	Scenario sim;
	auto	&low  = sim.add("low", "10.0.0.1", withId(100, false));
	auto	&high = sim.add("high", "10.0.0.2", withId(200, false));
	ASSERT_TRUE(sim.discover(low, high));

	FakeNet::LinkProfile profile;
	profile.latency = 5ms;
	sim.network->setProfile(profile);

	ASSERT_TRUE(low.engine->connect(high.id()));
	ASSERT_TRUE(high.engine->connect(low.id()));

	ASSERT_TRUE(sim.driver.runUntil([&] { return low.events.isConnectedTo(high.id()) && high.events.isConnectedTo(low.id()); }, 5s));
	EXPECT_TRUE(low.events.requests().empty()) << "Its application asked for this session itself";
	EXPECT_TRUE(high.events.requests().empty());
}


TEST(SessionSimTest, ConnectWhileInboundPending_Accepts)
{
	Scenario sim;
	auto	&a = sim.add("a", "10.0.0.1");
	auto	&b = sim.add("b", "10.0.0.2", withId(0, false));
	ASSERT_TRUE(sim.discover(a, b));

	ASSERT_TRUE(a.engine->connect(b.id()));
	ASSERT_TRUE(sim.driver.runUntil([&] { return b.events.requests().size() == 1; }, 1s));

	// B's application answers by asking for the same session
	ASSERT_TRUE(b.engine->connect(a.id()));

	ASSERT_TRUE(sim.driver.runUntil([&] { return a.events.isConnectedTo(b.id()) && b.events.isConnectedTo(a.id()); }, 1s));
	EXPECT_EQ(b.events.connected().size(), 1u);
	EXPECT_TRUE(b.events.ended().empty());
}


TEST(SessionSimTest, UnansweredRequest_IsDeclinedAfterAWhile)
{
	Scenario sim;
	auto	&a = sim.add("a", "10.0.0.1");
	auto	&b = sim.add("b", "10.0.0.2", withId(0, false));
	ASSERT_TRUE(sim.discover(a, b));

	const auto asked = sim.driver.elapsed();
	ASSERT_TRUE(a.engine->connect(b.id()));

	sim.driver.run(29s);
	EXPECT_TRUE(a.events.ended().empty()) << "The session is kept alive while B's application thinks about it";
	EXPECT_FALSE(a.engine->connect(b.id())) << "Asking twice does not help";

	ASSERT_TRUE(sim.driver.runUntil([&] { return !a.events.ended().empty() && !b.events.ended().empty(); }, 5s));
	EXPECT_GE(sim.driver.elapsed() - asked, 30s);
	EXPECT_EQ(a.events.ended(), std::vector<Ended>({{b.id(), DisconnectReason::Declined}}));
	EXPECT_EQ(b.events.ended(), std::vector<Ended>({{a.id(), DisconnectReason::Local}}));
}


TEST(SessionSimTest, ConnectToAPeerThatIsGone_EndsAsLost)
{
	Scenario sim;
	auto	&a = sim.add("a", "10.0.0.1");
	auto	&b = sim.add("b", "10.0.0.2");
	ASSERT_TRUE(sim.discover(a, b));

	sim.freeze(b);
	const auto asked = sim.driver.elapsed();
	ASSERT_TRUE(a.engine->connect(b.id()));

	ASSERT_TRUE(sim.driver.runUntil([&] { return !a.events.ended().empty(); }, 20s));
	EXPECT_EQ(a.events.ended(), std::vector<Ended>({{b.id(), DisconnectReason::Lost}})) << "Every connect() has exactly one outcome";
	EXPECT_GE(sim.driver.elapsed() - asked, Scenario::defaultConfig().timings.peerTimeout);
	EXPECT_TRUE(a.events.connected().empty());
}


TEST(SessionSimTest, HelloWithNewStream_ReplacesDeadSession)
{
	Scenario sim;
	auto	&a = sim.add("a", "10.0.0.1", withId(100));
	auto	&b = sim.add("b", "10.0.0.2", withId(200));
	ASSERT_TRUE(sim.connect(a, b));

	// A's side of the session is gone without B being told: its goodbye never arrives
	sim.network->setHostDown(b.ip, true);
	a.engine->disconnect(b.id());
	sim.driver.run(2s);
	sim.network->setHostDown(b.ip, false);

	ASSERT_EQ(a.events.ended(), std::vector<Ended>({{b.id(), DisconnectReason::Local}}));
	ASSERT_TRUE(b.events.ended().empty()) << "B still believes in the session";

	// A asks again: for B that is a Hello of a peer it is connected to, on a new stream
	ASSERT_TRUE(a.engine->connect(b.id()));
	ASSERT_TRUE(sim.driver.runUntil([&] { return a.events.connected().size() == 2 && b.events.connected().size() == 2; }, 3s));

	EXPECT_EQ(b.events.ended(), std::vector<Ended>({{a.id(), DisconnectReason::Lost}})) << "The old session ended before the new one began";

	const auto order = b.events.order();
	ASSERT_GE(order.size(), 2u);
	EXPECT_EQ(order[order.size() - 2].first, Kind::Disconnected);
	EXPECT_EQ(order.back().first, Kind::Connected);
}


TEST(SessionSimTest, RestartedPeer_IsANewPeer)
{
	Scenario sim;
	auto	&a		= sim.add("a", "10.0.0.1");
	auto	&before = sim.add("b", "10.0.0.2");
	ASSERT_TRUE(sim.connect(a, before));
	const PeerId oldId = before.id();

	// The machine of B is started again
	sim.freeze(before);
	before.engine.reset();
	auto &after = sim.add("b", "10.0.0.2");

	ASSERT_TRUE(sim.connect(a, after));
	EXPECT_NE(after.id(), oldId);
	EXPECT_EQ(a.engine->connectedPeers().size(), 2u) << "The old session is not replaced: it ends when its peer stays silent";

	ASSERT_TRUE(sim.driver.runUntil([&] { return !a.events.ended().empty(); }, 10s));
	EXPECT_EQ(a.events.ended(), std::vector<Ended>({{oldId, DisconnectReason::Lost}}));
	EXPECT_EQ(a.engine->connectedPeers(), std::vector<PeerId>{after.id()});
}


// ---------------------------------------------------------------------------
// Messages and the life of a session
// ---------------------------------------------------------------------------

TEST(SessionSimTest, ThreePeerMesh_KeepsTheOrderPerPeer)
{
	Scenario sim;
	auto	&a = sim.add("a", "10.0.0.1");
	auto	&b = sim.add("b", "10.0.0.2");
	auto	&c = sim.add("c", "10.0.0.3");
	ASSERT_TRUE(sim.connect(a, b));
	ASSERT_TRUE(sim.connect(b, c));
	ASSERT_TRUE(sim.connect(c, a));

	FakeNet::LinkProfile profile;
	profile.latency		= 2ms;
	profile.lossRate	= 0.05;
	profile.reorderRate = 0.1;
	sim.network->setProfile(profile);

	constexpr uint32_t					Messages = 300;
	const std::vector<Scenario::Node *> all{&a, &b, &c};

	for (uint32_t i = 0; i < Messages; ++i)
	{
		for (auto *from : all)
		{
			for (auto *to : all)
			{
				if (from != to)
				{
					ASSERT_EQ(from->engine->send(to->id(), i, payload(i % 7 == 0 ? 5000 : 40), Lane::Reliable), SendResult::Queued);
				}
			}
		}
	}

	ASSERT_TRUE(sim.driver.runUntil([&] { return std::ranges::all_of(all, [](const auto *node) { return node->received.size() == 2 * Messages; }); }, 60s));

	for (const auto *node : all)
	{
		std::map<PeerId, uint32_t> next;
		for (const auto &message : node->events.messages())
			ASSERT_EQ(message.type, next[message.from]++) << "Order of one sender broken at " << node->name;

		EXPECT_EQ(next.size(), 2u);
		EXPECT_TRUE(node->events.ended().empty());
	}
}


TEST(SessionSimTest, TenPeers_ExchangeAThousandMessagesEach)
{
	constexpr int				  Peers	   = 10;
	constexpr uint32_t			  Messages = 1000;

	Scenario					  sim;
	auto						 &hub = sim.add("hub", "10.0.0.1");

	std::vector<Scenario::Node *> peers;
	for (int i = 0; i < Peers; ++i)
	{
		peers.push_back(&sim.add("peer-" + std::to_string(i), "10.0.0." + std::to_string(10 + i)));
		ASSERT_TRUE(sim.connect(*peers.back(), hub));
	}

	for (uint32_t i = 0; i < Messages; ++i)
	{
		for (auto *peer : peers)
		{
			ASSERT_EQ(peer->engine->send(hub.id(), i, payload(200), Lane::Reliable), SendResult::Queued);
			ASSERT_EQ(hub.engine->send(peer->id(), i, payload(200), Lane::Reliable), SendResult::Queued);
		}
	}

	ASSERT_TRUE(sim.driver.runUntil(
		[&] { return hub.received.size() == Peers * Messages && std::ranges::all_of(peers, [](const auto *peer) { return peer->received.size() == Messages; }); }, 60s));

	std::map<PeerId, uint32_t> next;
	for (const auto &message : hub.events.messages())
		ASSERT_EQ(message.type, next[message.from]++);

	EXPECT_EQ(next.size(), static_cast<size_t>(Peers));
}


TEST(SessionSimTest, Connected_ComesBeforeTheFirstMessage_AndNothingFollowsDisconnected)
{
	Scenario sim;
	auto	&a = sim.add("a", "10.0.0.1");
	auto	&b = sim.add("b", "10.0.0.2");
	ASSERT_TRUE(sim.discover(a, b));

	FakeNet::LinkProfile profile;
	profile.latency = 5ms;
	sim.network->setProfile(profile);

	ASSERT_TRUE(a.engine->connect(b.id()));

	// A sends the moment it is connected: B's Accept is not confirmed yet when the data arrives
	ASSERT_TRUE(sim.driver.runUntil([&] { return a.events.isConnectedTo(b.id()); }, 1s));
	ASSERT_FALSE(b.events.isConnectedTo(a.id()));

	for (uint32_t i = 0; i < 200; ++i)
		ASSERT_EQ(a.engine->send(b.id(), i, payload(3000), Lane::Reliable), SendResult::Queued);

	sim.driver.run(100ms);
	b.engine->disconnect(a.id());
	sim.driver.run(5s);

	const auto order	 = b.events.order();
	const auto connected = std::ranges::find(order, std::pair{Kind::Connected, a.id()});
	const auto message	 = std::ranges::find(order, std::pair{Kind::Message, a.id()});

	ASSERT_NE(message, order.end()) << "Data that arrived before the Accept was confirmed is not lost";
	EXPECT_LT(connected, message);
	EXPECT_EQ(order.back().first, Kind::Disconnected);
	EXPECT_EQ(std::ranges::count(order, std::pair{Kind::Disconnected, a.id()}), 1);
	EXPECT_EQ(b.events.ended(), std::vector<Ended>({{a.id(), DisconnectReason::Local}}));
	EXPECT_EQ(a.events.ended(), std::vector<Ended>({{b.id(), DisconnectReason::Remote}}));
}


TEST(SessionSimTest, EarlyLaneData_IsNeverAcknowledged)
{
	Scenario sim;
	auto	&a = sim.add("a", "10.0.0.1");
	auto	&b = sim.add("b", "10.0.0.2", withId(0, false));
	ASSERT_TRUE(sim.discover(a, b));

	ASSERT_TRUE(a.engine->connect(b.id()));
	ASSERT_TRUE(sim.driver.runUntil([&] { return b.events.requests().size() == 1; }, 1s));

	// A is not connected: it cannot send. Someone who knows the stream IDs of the pending session can.
	uint32_t streamOfA = 0;
	uint32_t streamOfB = 0;
	size_t	 acksFromB = 0;

	sim.network->setTap(
		[&](const net::SocketAddress &from, const net::SocketAddress &, const std::span<const uint8_t> data)
		{
			const auto packet = channel::decodePacket(data);
			if (!packet)
				return;

			if (from.ip == ipv4("10.0.0.2"))
			{
				streamOfB = packet->header.srcStreamID;
				streamOfA = packet->header.dstStreamID;
				acksFromB += packet->header.flags.kind() == channel::PacketKind::Ack && packet->header.flags.lane() != channel::Lane::Control ? 1 : 0;
			}
		});

	sim.driver.run(2s); // the peers ask each other for signs of life: now the tap knows both stream IDs
	ASSERT_NE(streamOfB, 0u);

	channel::PacketHeader header;
	header.flags	   = channel::PacketFlags::data(channel::Lane::Reliable);
	header.srcStreamID = streamOfA;
	header.dstStreamID = streamOfB;
	header.seq		   = 1;
	header.tag		   = 5;

	// As if it came from A's own socket: the engine of A would never send this
	sim.network->inject(a.engine->localEndpoint(), b.engine->localEndpoint(), channel::encodePacket(header, payload(10)));

	sim.driver.run(2s);

	EXPECT_EQ(acksFromB, 0u) << "Before a session is connected nothing but its Control lane is answered";
	EXPECT_TRUE(b.received.empty());

	b.engine->accept(a.id());
	ASSERT_TRUE(sim.driver.runUntil([&] { return b.events.isConnectedTo(a.id()); }, 1s));
	EXPECT_TRUE(b.received.empty()) << "... and nothing of it was kept";
}


TEST(SessionSimTest, SlowConsumer_PausesAndResumesItsSender)
{
	Scenario	 sim;
	EngineConfig config = Scenario::defaultConfig();
	config.maxSendRate	= 0;

	auto &a				= sim.add("a", "10.0.0.1", config);
	auto &b				= sim.add("b", "10.0.0.2", config);
	ASSERT_TRUE(sim.connect(a, b));

	b.hold();

	constexpr size_t   MessageSize = 1024 * 1024;
	constexpr uint32_t Messages	   = 2 * channel::BacklogPauseBytes / MessageSize;

	for (uint32_t i = 0; i < Messages; ++i)
		ASSERT_EQ(a.engine->send(b.id(), i, payload(MessageSize), Lane::Bulk), SendResult::Queued);

	sim.driver.run(30s);

	EXPECT_LT(b.received.size(), Messages) << "B's application takes nothing: A is held back";
	EXPECT_GE(b.received.size() * MessageSize, channel::BacklogPauseBytes);
	EXPECT_LE(b.received.size() * MessageSize, channel::BacklogPauseBytes + 2 * MessageSize);
	EXPECT_TRUE(a.events.ended().empty()) << "A paused peer is not a lost peer";
	EXPECT_GT(a.engine->stats(b.id())->bytesQueued, 0u);

	// Media is not paused
	const size_t before = b.received.size();
	for (uint32_t i = 0; i < 10; ++i)
		ASSERT_EQ(a.engine->send(b.id(), 9000 + i, payload(100), Lane::Media), SendResult::Queued);

	sim.driver.run(1s);
	EXPECT_EQ(b.received.size(), before + 10);

	b.release();

	ASSERT_TRUE(sim.driver.runUntil([&] { return b.received.size() == Messages + 10; }, 60s));
	EXPECT_TRUE(a.engine->flush(b.id(), 0ms));
}


TEST(SessionSimTest, Disconnect_SendsWhatIsWaitingFirst)
{
	Scenario sim;
	auto	&a = sim.add("a", "10.0.0.1");
	auto	&b = sim.add("b", "10.0.0.2");
	ASSERT_TRUE(sim.connect(a, b));

	FakeNet::LinkProfile profile;
	profile.latency = 5ms;
	sim.network->setProfile(profile);

	for (uint32_t i = 0; i < 50; ++i)
		ASSERT_EQ(a.engine->send(b.id(), i, payload(2000), Lane::Reliable), SendResult::Queued);

	a.engine->disconnect(b.id());
	EXPECT_EQ(a.engine->send(b.id(), 99, payload(10), Lane::Reliable), SendResult::NotConnected) << "Nothing new is taken";
	EXPECT_TRUE(a.engine->connectedPeers().empty());

	ASSERT_TRUE(sim.driver.runUntil([&] { return !a.events.ended().empty() && !b.events.ended().empty(); }, 5s));

	EXPECT_EQ(b.received.size(), 50u) << "Everything that was accepted before arrived";
	EXPECT_EQ(b.events.order().back().first, Kind::Disconnected);
	EXPECT_LT(sim.driver.elapsed(), 12s);
}


TEST(SessionSimTest, Disconnect_GivesUpDrainingAfterASecond)
{
	Scenario sim;
	auto	&a = sim.add("a", "10.0.0.1");
	auto	&b = sim.add("b", "10.0.0.2");
	ASSERT_TRUE(sim.connect(a, b));

	sim.freeze(b);
	ASSERT_EQ(a.engine->send(b.id(), 1, payload(2000), Lane::Reliable), SendResult::Queued);

	const auto asked = sim.driver.elapsed();
	a.engine->disconnect(b.id());

	ASSERT_TRUE(sim.driver.runUntil([&] { return !a.events.ended().empty(); }, 5s));
	EXPECT_EQ(sim.driver.elapsed() - asked, 1s);
	EXPECT_EQ(a.events.ended(), std::vector<Ended>({{b.id(), DisconnectReason::Local}}));

	// The goodbye is not waited for either
	ASSERT_TRUE(sim.driver.runUntil([&] { return a.engine->connect(b.id()); }, 1s));
	EXPECT_LE(sim.driver.elapsed() - asked, 1s + 250ms);
}


TEST(SessionSimTest, Shutdown_TellsEveryPeerAndFinishes)
{
	Scenario sim;
	auto	&a = sim.add("a", "10.0.0.1");
	auto	&b = sim.add("b", "10.0.0.2");
	auto	&c = sim.add("c", "10.0.0.3", withId(0, false));
	ASSERT_TRUE(sim.connect(a, b));
	ASSERT_TRUE(sim.discover(a, c));
	ASSERT_TRUE(a.engine->connect(c.id()));
	ASSERT_TRUE(sim.driver.runUntil([&] { return c.events.requests().size() == 1; }, 1s));

	a.engine->shutdown();
	ASSERT_TRUE(sim.driver.runUntil([&] { return a.engine->finished(); }, 1s));

	EXPECT_TRUE(std::ranges::is_permutation(a.events.ended(), std::vector<Ended>{{b.id(), DisconnectReason::Shutdown}, {c.id(), DisconnectReason::Shutdown}}))
		<< "The session and the request that was still open both have their outcome";
	EXPECT_EQ(b.events.ended(), std::vector<Ended>({{a.id(), DisconnectReason::Remote}}));
	EXPECT_EQ(a.engine->send(b.id(), 1, payload(10), Lane::Reliable), SendResult::NotRunning);
	EXPECT_FALSE(a.engine->isRunning());

	// The request that was still open at C has its outcome as well
	sim.driver.run(1s);
	EXPECT_EQ(c.events.ended(), std::vector<Ended>({{a.id(), DisconnectReason::Remote}}));
}


TEST(SessionSimTest, IdleTraffic_IsABeaconEveryTwoSecondsAndOnePingPerSession)
{
	Scenario sim;
	auto	&a = sim.add("a", "10.0.0.1");
	auto	&b = sim.add("b", "10.0.0.2");
	auto	&c = sim.add("c", "10.0.0.3");
	ASSERT_TRUE(sim.connect(a, b));
	ASSERT_TRUE(sim.connect(a, c));
	sim.driver.run(5s);

	std::map<std::string, int> beacons;
	int						   pings = 0;
	int						   acks	 = 0;
	int						   other = 0;

	sim.network->setTap(
		[&](const net::SocketAddress &from, const net::SocketAddress &, const std::span<const uint8_t> data)
		{
			const auto seen = look(data);

			if (seen.isBeacon)
				++beacons[from.ip.toString()];
			else if (seen.kind == channel::PacketKind::Ping)
				++pings;
			else if (seen.kind == channel::PacketKind::Ack)
				++acks;
			else
				++other;
		});

	sim.driver.run(60s);

	for (const auto &ip : {"10.0.0.1", "10.0.0.2", "10.0.0.3"})
		EXPECT_NEAR(beacons[ip], 30, 1) << ip;

	EXPECT_NEAR(pings, 2 * 60, 4) << "Two idle sessions: one question per second in each";
	EXPECT_EQ(acks, pings) << "... and its answer";
	EXPECT_EQ(other, 0);
}


// ---------------------------------------------------------------------------
// Strangers
// ---------------------------------------------------------------------------

TEST(SessionSimTest, Strangers_AllocateNothingBeyondARegistryRowOrAPendingLink)
{
	Scenario sim;
	auto	&victim = sim.add("victim", "10.0.0.1", withId(1, false));
	sim.driver.run(1s);

	auto stranger = sim.network->factory("10.0.0.66")({ipv4("10.0.0.66"), 0}, {});
	ASSERT_TRUE(stranger.has_value());

	const auto &identity = Scenario::defaultConfig();
	const auto	appHash	 = discovery::hashAppId(identity.appId);
	const auto	target	 = victim.engine->localEndpoint();

	// Far more announcements than the registry holds, each under another ID
	for (uint64_t id = 1000; id < 1000 + 2 * internal::MaxDiscoveredPeers; ++id)
		static_cast<void>((*stranger)->sendTo(target, discovery::encodeBeacon({.instanceId = id, .appIdHash = appHash, .version = {1, 0}, .reply = true, .name = "x"})));

	sim.driver.run(100ms);
	EXPECT_EQ(victim.engine->peers().size(), internal::MaxDiscoveredPeers);

	// ... and far more Hellos than sessions may be pending
	for (uint64_t id = 5000; id < 5000 + 4 * internal::MaxPendingSessions; ++id)
	{
		channel::PacketHeader header;
		header.flags	   = channel::PacketFlags::data(channel::Lane::Control);
		header.srcStreamID = static_cast<uint32_t>(id);
		header.seq		   = 1;
		header.tag		   = std::to_underlying(session::ControlType::Hello);

		const auto hello   = session::encode(session::Hello{.fromInstance = id, .toInstance = victim.id().value, .appIdHash = appHash, .verMajor = 1, .verMinor = 0, .name = "x"});
		static_cast<void>((*stranger)->sendTo(target, channel::encodePacket(header, hello)));
	}

	sim.driver.run(100ms);
	EXPECT_EQ(victim.events.requests().size(), internal::MaxPendingSessions) << "Further requests are ignored until these are answered or given up";

	// Whoever does not follow up is forgotten
	sim.driver.run(40s);
	EXPECT_EQ(victim.events.ended().size(), internal::MaxPendingSessions);
	EXPECT_TRUE(victim.engine->peers().empty());

	// A real peer still gets through
	auto &peer = sim.add("peer", "10.0.0.2");
	ASSERT_TRUE(sim.discover(peer, victim));
	ASSERT_TRUE(peer.engine->connect(victim.id()));
	ASSERT_TRUE(sim.driver.runUntil([&] { return std::ranges::any_of(victim.events.requests(), [&](const PeerInfo &info) { return info.id == peer.id(); }); }, 1s));
}

} // namespace SimTests
