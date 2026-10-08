#include <gtest/gtest.h>

#include <chrono>
#include <vector>

#include "Discovery/DiscoveryLogic.h"
#include "TestIp.h"

using namespace netlink;
using namespace netlink::discovery;
using namespace std::chrono_literals;


namespace DiscoveryTests
{

class DiscoveryLogicTest : public ::testing::Test
{
protected:
	using Sighting = DiscoveryLogic::Sighting;

	void SetUp() override
	{
		logic.setInterface(ipv4("10.0.0.1"), ipv4("255.255.255.0"));
		logic.setAnnouncing(true);
	}

	static LocalIdentity identityOf(const uint64_t id, const std::string &name = "peer")
	{
		return {.instanceId = id, .appIdHash = hashAppId("app"), .version = {1, 2}, .name = name};
	}

	static std::vector<uint8_t> beaconOf(const LocalIdentity &identity, const bool reply = false)
	{
		return encodeBeacon({.instanceId = identity.instanceId, .appIdHash = identity.appIdHash, .version = identity.version, .reply = reply, .name = identity.name});
	}

	DiscoveryLogic::Seen see(const uint64_t id, const char *ip = "10.0.0.2", const uint16_t port = 4000, const bool reply = false)
	{
		return logic.onBeacon({ipv4(ip), port}, beaconOf(identityOf(id), reply), now);
	}

	static bool				  noSessions(PeerId) { return false; }

	DiscoveryLogic			  logic{identityOf(1, "me")};
	DiscoveryLogic::TimePoint now = DiscoveryLogic::Clock::now();
};


TEST_F(DiscoveryLogicTest, FirstSighting_IsNewAndAnswered)
{
	const auto seen = see(2);

	ASSERT_EQ(seen.sighting, Sighting::New);
	EXPECT_TRUE(seen.reply) << "The newcomer does not have to wait for the next announcement";
	EXPECT_EQ(seen.peer->info.id, PeerId{2});
	EXPECT_EQ(seen.peer->info.displayName, "peer");
	EXPECT_EQ(seen.peer->info.address, "10.0.0.2");
	EXPECT_EQ(seen.peer->info.port, 4000);
	EXPECT_EQ(seen.peer->info.appVersion, "1.2");
	EXPECT_EQ(seen.peer->endpoint, (net::SocketAddress{ipv4("10.0.0.2"), 4000}));

	const auto again = see(2);
	EXPECT_EQ(again.sighting, Sighting::Known);
	EXPECT_FALSE(again.reply);
	EXPECT_EQ(logic.peers().size(), 1u);
}


TEST_F(DiscoveryLogicTest, Reply_IsNotAnsweredAgain)
{
	const auto seen = see(2, "10.0.0.2", 4000, true);

	EXPECT_EQ(seen.sighting, Sighting::New);
	EXPECT_FALSE(seen.reply) << "Two engines would answer each other forever";
}


TEST_F(DiscoveryLogicTest, NotAnnouncing_AnswersNobody)
{
	logic.setAnnouncing(false);

	const auto seen = see(2);
	EXPECT_EQ(seen.sighting, Sighting::New) << "Others are still found";
	EXPECT_FALSE(seen.reply);
}


TEST_F(DiscoveryLogicTest, PeerAtANewAddress_IsReportedAgain)
{
	see(2);

	const auto moved = see(2, "10.0.0.2", 4001);
	EXPECT_EQ(moved.sighting, Sighting::New);
	EXPECT_FALSE(moved.reply) << "It knows this engine already";
	EXPECT_EQ(logic.find(PeerId{2})->endpoint.port, 4001);
	EXPECT_EQ(logic.size(), 1u);
}


TEST_F(DiscoveryLogicTest, Filters_RunBeforeAnythingIsKept)
{
	const net::SocketAddress from{ipv4("10.0.0.2"), 4000};

	EXPECT_EQ(see(1).sighting, Sighting::Ignored) << "Its own announcement";
	EXPECT_EQ(see(2, "10.0.1.2").sighting, Sighting::Ignored) << "Another subnet";

	LocalIdentity otherApp = identityOf(3);
	otherApp.appIdHash	   = hashAppId("another app");
	EXPECT_EQ(logic.onBeacon(from, beaconOf(otherApp), now).sighting, Sighting::Ignored);

	LocalIdentity otherVersion = identityOf(4);
	otherVersion.version	   = {1, 3};
	EXPECT_EQ(logic.onBeacon(from, beaconOf(otherVersion), now).sighting, Sighting::Ignored);

	EXPECT_EQ(logic.onBeacon(from, beaconOf(identityOf(5, std::string(MaxDisplayName + 1, 'x'))), now).sighting, Sighting::Ignored);

	const std::vector<uint8_t> junk{1, 2, 3};
	EXPECT_EQ(logic.onBeacon(from, junk, now).sighting, Sighting::Ignored);

	EXPECT_EQ(logic.size(), 0u);
}


TEST_F(DiscoveryLogicTest, WithoutANetmask_EverySubnetIsTaken)
{
	logic.setInterface(ipv4("127.0.0.1"), {});

	EXPECT_EQ(see(2, "127.0.0.2").sighting, Sighting::New);
	EXPECT_EQ(see(3, "192.168.1.1").sighting, Sighting::New);
	EXPECT_TRUE(logic.broadcastAddress().isBroadcast());
}


TEST_F(DiscoveryLogicTest, Announcements_GoToTheSubnet)
{
	EXPECT_EQ(logic.broadcastAddress(), ipv4("10.0.0.255"));

	const auto beacon = decodeBeacon(logic.beacon(true));
	ASSERT_TRUE(beacon.has_value());
	EXPECT_EQ(beacon->instanceId, 1u);
	EXPECT_EQ(beacon->name, "me");
	EXPECT_TRUE(beacon->reply);
}


TEST_F(DiscoveryLogicTest, SilentPeers_ExpireUnlessASessionExists)
{
	see(2);
	see(3);

	now += internal::PeerExpiry - 1ms;
	EXPECT_TRUE(logic.expire(now, noSessions).empty());

	see(3); // still announcing

	now += 1ms;
	EXPECT_EQ(logic.expire(now, [](const PeerId id) { return id == PeerId{2}; }), std::vector<PeerId>{}) << "The peer of a session may go quiet";
	EXPECT_EQ(logic.expire(now, noSessions), std::vector<PeerId>{PeerId{2}});
	EXPECT_EQ(logic.size(), 1u);

	EXPECT_EQ(see(2).sighting, Sighting::New) << "A peer that comes back is discovered again";
}


TEST_F(DiscoveryLogicTest, Registry_IsBounded)
{
	for (uint64_t id = 10; id < 10 + internal::MaxDiscoveredPeers; ++id)
		ASSERT_EQ(see(id).sighting, Sighting::New);

	EXPECT_EQ(see(5).sighting, Sighting::Ignored) << "No room for one more";
	EXPECT_EQ(see(10).sighting, Sighting::Known);
	EXPECT_EQ(logic.size(), internal::MaxDiscoveredPeers);

	EXPECT_EQ(logic.clear().size(), internal::MaxDiscoveredPeers);
	EXPECT_EQ(logic.size(), 0u);
}

} // namespace DiscoveryTests
