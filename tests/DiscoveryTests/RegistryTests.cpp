#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "TestIp.h"
#include "Discovery/DiscoveryRegistry.h"

using namespace std::chrono_literals;
using netlink::discovery::DiscoveryRegistry;


namespace DiscoveryTests
{

static DiscoveryEndpoint makeEndpoint(std::string_view ip = "10.0.0.6", int port = 6001, const std::string &name = "pc-b")
{
	return DiscoveryEndpoint{ipv4(ip), port, name};
}


// ---------------------------------------------------------------------------
// addOrUpdate
// ---------------------------------------------------------------------------

TEST(DiscoveryRegistry, AddOrUpdate_ReturnsAddedForNewPeer)
{
	DiscoveryRegistry registry;

	auto			  result = registry.addOrUpdate(makeEndpoint());
	EXPECT_EQ(result, DiscoveryRegistry::UpdateResult::Added) << "Adding a peer that was not previously known must report Added";
	EXPECT_EQ(registry.size(), 1u);
}


TEST(DiscoveryRegistry, AddOrUpdate_ReturnsUpdatedForExistingPeer)
{
	DiscoveryRegistry registry;

	auto			  endpoint = makeEndpoint();
	registry.addOrUpdate(endpoint);

	auto result = registry.addOrUpdate(endpoint);
	EXPECT_EQ(result, DiscoveryRegistry::UpdateResult::Refreshed) << "Adding an already-known peer again must report Refreshed, not Added or Updated";
	EXPECT_EQ(registry.size(), 1u) << "Re-adding an existing peer must not create a duplicate entry";
}


TEST(DiscoveryRegistry, AddOrUpdate_RefreshesLastSeenTimestamp)
{
	DiscoveryRegistry registry;

	auto			  endpoint = makeEndpoint();
	auto			  t0	   = std::chrono::steady_clock::now();
	registry.addOrUpdate(endpoint, t0);

	auto t1 = t0 + 500ms;
	registry.addOrUpdate(endpoint, t1);

	auto found = registry.find(endpoint);
	ASSERT_TRUE(found.has_value());
	EXPECT_EQ(found->lastSeen, t1) << "Re-adding an existing peer must refresh its lastSeen timestamp to the newly supplied time";
}


TEST(DiscoveryRegistry, AddOrUpdate_RefreshesMetadata)
{
	DiscoveryRegistry registry;

	registry.addOrUpdate(makeEndpoint("10.0.0.6", 6001, "pc-b"));
	registry.addOrUpdate(makeEndpoint("10.0.0.6", 6001, "pc-b-renamed"));

	auto found = registry.findByIP(ipv4("10.0.0.6"));
	ASSERT_TRUE(found.has_value());
	EXPECT_EQ(found->endpoint.displayName, "pc-b-renamed") << "Updating an existing peer must refresh its metadata (e.g. displayName)";
}


TEST(DiscoveryRegistry, AddOrUpdate_TreatsDifferentPortAsDifferentPeer)
{
	DiscoveryRegistry registry;

	registry.addOrUpdate(makeEndpoint("10.0.0.6", 6001, "pc-b"));
	registry.addOrUpdate(makeEndpoint("10.0.0.6", 6002, "pc-b"));

	EXPECT_EQ(registry.size(), 1u) << "Endpoints with the same IP but different ports must be same peer but updated";
}


// ---------------------------------------------------------------------------
// touch
// ---------------------------------------------------------------------------

TEST(DiscoveryRegistry, Touch_ReturnsFalseForUnknownPeer)
{
	DiscoveryRegistry registry;

	EXPECT_FALSE(registry.touch(makeEndpoint())) << "Touching a peer that was never added must return false and be a no-op";
}


TEST(DiscoveryRegistry, Touch_ReturnsTrueAndRefreshesTimestampForKnownPeer)
{
	DiscoveryRegistry registry;

	auto			  endpoint = makeEndpoint();
	auto			  t0	   = std::chrono::steady_clock::now();
	registry.addOrUpdate(endpoint, t0);

	auto t1 = t0 + 1s;
	EXPECT_TRUE(registry.touch(endpoint, t1)) << "Touching a known peer must return true";

	auto found = registry.find(endpoint);
	ASSERT_TRUE(found.has_value());
	EXPECT_EQ(found->lastSeen, t1) << "touch() must update lastSeen to the provided timestamp";
}


TEST(DiscoveryRegistry, Touch_DoesNotModifyMetadata)
{
	DiscoveryRegistry registry;

	auto			  endpoint = makeEndpoint("10.0.0.6", 6001, "pc-b");
	registry.addOrUpdate(endpoint);
	registry.touch(endpoint);

	auto found = registry.find(endpoint);
	ASSERT_TRUE(found.has_value());
	EXPECT_EQ(found->endpoint.displayName, "pc-b") << "touch() must only refresh the timestamp, leaving stored metadata untouched";
}


// ---------------------------------------------------------------------------
// remove
// ---------------------------------------------------------------------------

TEST(DiscoveryRegistry, Remove_ReturnsFalseForUnknownPeer)
{
	DiscoveryRegistry registry;

	EXPECT_FALSE(registry.remove(makeEndpoint())) << "Removing a peer that was never added must return false";
}


TEST(DiscoveryRegistry, Remove_ReturnsTrueAndErasesKnownPeer)
{
	DiscoveryRegistry registry;

	auto			  endpoint = makeEndpoint();
	registry.addOrUpdate(endpoint);

	EXPECT_TRUE(registry.remove(endpoint)) << "Removing a known peer must return true";
	EXPECT_FALSE(registry.contains(endpoint)) << "After removal, the peer must no longer be reported as contained";
	EXPECT_EQ(registry.size(), 0u);
}


TEST(DiscoveryRegistry, Remove_OnlyRemovesMatchingEndpoint)
{
	DiscoveryRegistry registry;

	auto			  a = makeEndpoint("10.0.0.6", 6001, "pc-b");
	auto			  b = makeEndpoint("10.0.0.7", 6002, "pc-c");
	registry.addOrUpdate(a);
	registry.addOrUpdate(b);

	registry.remove(a);

	EXPECT_FALSE(registry.contains(a));
	EXPECT_TRUE(registry.contains(b)) << "Removing one peer must not affect other unrelated peers in the registry";
}


// ---------------------------------------------------------------------------
// removeStale
// ---------------------------------------------------------------------------

TEST(DiscoveryRegistry, RemoveStale_ReturnsEmptyWhenNoPeersAreStale)
{
	DiscoveryRegistry registry;

	auto			  now = std::chrono::steady_clock::now();
	registry.addOrUpdate(makeEndpoint(), now);

	auto removed = registry.removeStale(5s, now + 1s);
	EXPECT_TRUE(removed.empty()) << "removeStale() must not remove peers whose lastSeen is within the timeout window";
	EXPECT_EQ(registry.size(), 1u);
}


TEST(DiscoveryRegistry, RemoveStale_RemovesPeersOlderThanTimeout)
{
	DiscoveryRegistry registry;

	auto			  endpoint = makeEndpoint();
	auto			  now	   = std::chrono::steady_clock::now();
	registry.addOrUpdate(endpoint, now);

	auto removed = registry.removeStale(5s, now + 10s);
	ASSERT_EQ(removed.size(), 1u) << "A peer whose lastSeen exceeds the timeout must be reported as removed";
	EXPECT_EQ(removed[0], endpoint);
	EXPECT_EQ(registry.size(), 0u) << "A stale peer must be erased from the registry";
}


TEST(DiscoveryRegistry, RemoveStale_OnlyRemovesStalePeersKeepingFreshOnes)
{
	DiscoveryRegistry registry;

	auto			  now	= std::chrono::steady_clock::now();
	auto			  stale = makeEndpoint("10.0.0.6", 6001, "stale-pc");
	auto			  fresh = makeEndpoint("10.0.0.7", 6002, "fresh-pc");

	registry.addOrUpdate(stale, now);
	registry.addOrUpdate(fresh, now + 8s);

	auto removed = registry.removeStale(5s, now + 10s);

	ASSERT_EQ(removed.size(), 1u);
	EXPECT_EQ(removed[0], stale) << "Only the peer exceeding the timeout must be removed";
	EXPECT_TRUE(registry.contains(fresh)) << "A peer still within the timeout window must remain in the registry";
}


TEST(DiscoveryRegistry, RemoveStale_OnEmptyRegistryReturnsEmpty)
{
	DiscoveryRegistry registry;

	auto			  removed = registry.removeStale(5s);
	EXPECT_TRUE(removed.empty()) << "Calling removeStale() on an empty registry must return an empty list and not throw";
}


// ---------------------------------------------------------------------------
// contains / find / findByIP
// ---------------------------------------------------------------------------

TEST(DiscoveryRegistry, Contains_FalseForUnknownPeer)
{
	DiscoveryRegistry registry;

	EXPECT_FALSE(registry.contains(makeEndpoint())) << "An empty registry must not report containing any peer";
}


TEST(DiscoveryRegistry, Contains_TrueAfterAdd)
{
	DiscoveryRegistry registry;

	auto			  endpoint = makeEndpoint();
	registry.addOrUpdate(endpoint);

	EXPECT_TRUE(registry.contains(endpoint)) << "After adding a peer, contains() must report true for it";
}


TEST(DiscoveryRegistry, Find_ReturnsNulloptForUnknownPeer)
{
	DiscoveryRegistry registry;

	EXPECT_FALSE(registry.find(makeEndpoint()).has_value()) << "find() must return std::nullopt for a peer that was never added";
}


TEST(DiscoveryRegistry, Find_ReturnsKnownPeerWithEndpointAndTimestamp)
{
	DiscoveryRegistry registry;

	auto			  endpoint = makeEndpoint();
	auto			  now	   = std::chrono::steady_clock::now();
	registry.addOrUpdate(endpoint, now);

	auto found = registry.find(endpoint);
	ASSERT_TRUE(found.has_value());
	EXPECT_EQ(found->endpoint, endpoint);
	EXPECT_EQ(found->lastSeen, now);
}


TEST(DiscoveryRegistry, FindByIP_ReturnsNulloptForUnknownIP)
{
	DiscoveryRegistry registry;

	EXPECT_FALSE(registry.findByIP(ipv4("192.168.99.99")).has_value()) << "findByIP() must return std::nullopt when no peer with the given IP is known";
}


TEST(DiscoveryRegistry, FindByIP_ReturnsPeerMatchingIPRegardlessOfPort)
{
	DiscoveryRegistry registry;

	auto			  endpoint = makeEndpoint("10.0.0.6", 6001, "pc-b");
	registry.addOrUpdate(endpoint);

	auto found = registry.findByIP(ipv4("10.0.0.6"));
	ASSERT_TRUE(found.has_value());
	EXPECT_EQ(found->endpoint.displayName, "pc-b");
}


// ---------------------------------------------------------------------------
// snapshot / size / clear
// ---------------------------------------------------------------------------

TEST(DiscoveryRegistry, Snapshot_EmptyForNewRegistry)
{
	DiscoveryRegistry registry;

	EXPECT_TRUE(registry.snapshot().empty()) << "A freshly constructed registry must produce an empty snapshot";
}


TEST(DiscoveryRegistry, Snapshot_ContainsAllAddedPeers)
{
	DiscoveryRegistry registry;

	registry.addOrUpdate(makeEndpoint("10.0.0.6", 6001, "pc-b"));
	registry.addOrUpdate(makeEndpoint("10.0.0.7", 6002, "pc-c"));

	auto snapshot = registry.snapshot();
	EXPECT_EQ(snapshot.size(), 2u) << "snapshot() must return an entry for every peer currently tracked by the registry";
}


TEST(DiscoveryRegistry, Snapshot_IsACopyNotAffectedByLaterMutations)
{
	DiscoveryRegistry registry;

	registry.addOrUpdate(makeEndpoint());
	auto snapshot = registry.snapshot();

	registry.addOrUpdate(makeEndpoint("10.0.0.7", 6002, "pc-c"));

	EXPECT_EQ(snapshot.size(), 1u) << "A previously taken snapshot must not reflect peers added to the registry afterwards";
}


TEST(DiscoveryRegistry, Size_ReflectsNumberOfKnownPeers)
{
	DiscoveryRegistry registry;

	EXPECT_EQ(registry.size(), 0u);

	registry.addOrUpdate(makeEndpoint("10.0.0.6", 6001, "pc-b"));
	EXPECT_EQ(registry.size(), 1u);

	registry.addOrUpdate(makeEndpoint("10.0.0.7", 6002, "pc-c"));
	EXPECT_EQ(registry.size(), 2u);
}


TEST(DiscoveryRegistry, Clear_RemovesAllPeers)
{
	DiscoveryRegistry registry;

	registry.addOrUpdate(makeEndpoint("10.0.0.6", 6001, "pc-b"));
	registry.addOrUpdate(makeEndpoint("10.0.0.7", 6002, "pc-c"));

	registry.clear();

	EXPECT_EQ(registry.size(), 0u) << "clear() must remove every peer tracked by the registry";
	EXPECT_TRUE(registry.snapshot().empty());
}


TEST(DiscoveryRegistry, Clear_OnEmptyRegistryDoesNotThrow)
{
	DiscoveryRegistry registry;

	EXPECT_NO_THROW(registry.clear()) << "clear() must be safe to call on a registry that has no peers";
}

} // namespace DiscoveryTests
