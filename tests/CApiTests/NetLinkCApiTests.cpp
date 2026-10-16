#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "NetLink/NetLinkC.h"

using namespace std::chrono_literals;


namespace CApiTests
{

// Kept off NetLink's default port and the one of the C++ API tests
constexpr uint16_t DiscoveryPort = 45462;


template <typename Predicate>
bool waitUntilTrue(Predicate predicate, const std::chrono::milliseconds timeout = 10s)
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;

	while (std::chrono::steady_clock::now() < deadline)
	{
		if (predicate())
			return true;

		std::this_thread::sleep_for(20ms);
	}

	return predicate();
}


std::vector<NLNetworkAdapter> adaptersOf(const NLNetLink *netlink)
{
	std::vector<NLNetworkAdapter> adapters(static_cast<size_t>(std::max(nl_get_adapters(netlink, nullptr, 0), 0)));
	const int32_t				  count = nl_get_adapters(netlink, adapters.data(), static_cast<int32_t>(adapters.size()));
	adapters.resize(static_cast<size_t>(std::clamp(count, 0, static_cast<int32_t>(adapters.size()))));
	return adapters;
}


// What one instance was told through its callbacks
struct Seen
{
	NLCallbacks callbacks()
	{
		NLCallbacks result{};
		result.context			= this;
		result.onPeerDiscovered = [](void *context, const NLPeerInfo *peer)
		{
			auto					   *self = static_cast<Seen *>(context);
			std::lock_guard<std::mutex> lock(self->mutex);
			self->discovered.emplace_back(peer->displayName, peer->id);
		};
		result.onConnected = [](void *context, const NLPeerInfo *)
		{
			auto					   *self = static_cast<Seen *>(context);
			std::lock_guard<std::mutex> lock(self->mutex);
			++self->connected;
		};
		result.onMessage = [](void *context, uint64_t, int32_t lane, uint32_t type, const uint8_t *data, size_t size)
		{
			auto					   *self = static_cast<Seen *>(context);
			std::lock_guard<std::mutex> lock(self->mutex);
			self->messages.push_back({lane, type, std::vector<uint8_t>(data, data + size)});
		};
		result.onLog = [](void *context, int32_t, const char *message)
		{
			auto					   *self = static_cast<Seen *>(context);
			std::lock_guard<std::mutex> lock(self->mutex);
			self->logLines.emplace_back(message);
		};
		return result;
	}

	std::optional<uint64_t> peerNamed(const std::string &name)
	{
		std::lock_guard<std::mutex> lock(mutex);
		const auto					it = std::ranges::find_if(discovered, [&](const auto &peer) { return peer.first == name; });
		return it != discovered.end() ? std::optional(it->second) : std::nullopt;
	}

	int connectedCount()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return connected;
	}

	size_t messageCount()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return messages.size();
	}

	struct Received
	{
		int32_t				 lane;
		uint32_t			 type;
		std::vector<uint8_t> data;
	};

	std::mutex										  mutex;
	std::vector<std::pair<std::string, uint64_t>> discovered;
	int												  connected{0};
	std::vector<Received>							  messages;
	std::vector<std::string>						  logLines;
};


TEST(NetLinkCApi, Version_IsReported)
{
	ASSERT_NE(nl_version(), nullptr);
	EXPECT_GT(std::strlen(nl_version()), 0u);
}


TEST(NetLinkCApi, NullHandle_IsHarmless)
{
	NLPeerInfo		 peer{};
	NLNetworkAdapter adapter{};
	NLPeerStats		 stats{};
	uint64_t		 id = 0;
	const uint8_t	 byte = 1;

	EXPECT_FALSE(nl_start(nullptr, nullptr, nullptr));
	EXPECT_FALSE(nl_start_discovery(nullptr));
	EXPECT_EQ(nl_get_peers(nullptr, &peer, 1), 0);
	EXPECT_FALSE(nl_connect(nullptr, 1));
	EXPECT_EQ(nl_get_connected_peers(nullptr, &id, 1), 0);
	EXPECT_EQ(nl_send(nullptr, 1, 1, &byte, 1, NL_LANE_RELIABLE, 0), NL_SEND_INVALID);
	EXPECT_EQ(nl_broadcast(nullptr, 1, &byte, 1, NL_LANE_RELIABLE), 0u);
	EXPECT_FALSE(nl_get_stats(nullptr, 1, &stats));
	EXPECT_EQ(nl_get_adapters(nullptr, &adapter, 1), 0);
	EXPECT_FALSE(nl_set_active_adapter(nullptr, 1));
	EXPECT_EQ(nl_get_active_adapter_id(nullptr), 0u);

	nl_stop(nullptr);
	nl_stop_discovery(nullptr);
	nl_accept(nullptr, 1);
	nl_decline(nullptr, 1);
	nl_disconnect(nullptr, 1);
	nl_destroy(nullptr);
}


TEST(NetLinkCApi, Start_WithoutAppId_Fails)
{
	NLNetLink *netlink = nl_create();
	ASSERT_NE(netlink, nullptr);

	NLConfig config{};
	config.displayName = "no-app-id";

	EXPECT_FALSE(nl_start(netlink, &config, nullptr));
	EXPECT_FALSE(nl_start(netlink, nullptr, nullptr));

	nl_destroy(netlink);
}


TEST(NetLinkCApi, Adapters_AreListedBeforeStart)
{
	NLNetLink *netlink = nl_create();
	ASSERT_NE(netlink, nullptr);

	const int32_t total = nl_get_adapters(netlink, nullptr, 0);
	EXPECT_GE(total, 0);

	const auto adapters = adaptersOf(netlink);
	EXPECT_EQ(static_cast<int32_t>(adapters.size()), total);

	for (const auto &adapter : adapters)
	{
		EXPECT_NE(adapter.id, 0u);
		EXPECT_GE(adapter.priority, NL_ADAPTER_SUPPRESSED);
		EXPECT_LE(adapter.priority, NL_ADAPTER_PREFERRED);
		EXPECT_LT(std::strlen(adapter.adapterName), static_cast<size_t>(NL_MAX_STRING));
	}

	// Only what fits is copied, the count stays the total
	if (total > 1)
	{
		NLNetworkAdapter first{};
		EXPECT_EQ(nl_get_adapters(netlink, &first, 1), total);
		EXPECT_EQ(first.id, adapters.front().id);
	}

	nl_destroy(netlink);
}


TEST(NetLinkCApi, SetActiveAdapter_UnknownId_Fails)
{
	NLNetLink *netlink = nl_create();
	ASSERT_NE(netlink, nullptr);

	EXPECT_FALSE(nl_set_active_adapter(netlink, 0));
	EXPECT_EQ(nl_get_active_adapter_id(netlink), 0u);

	nl_destroy(netlink);
}


TEST(NetLinkCApi, Send_InvalidArguments_AreRejected)
{
	NLNetLink	 *netlink = nl_create();
	const uint8_t byte	  = 1;
	ASSERT_NE(netlink, nullptr);

	EXPECT_EQ(nl_send(netlink, 1, 1, nullptr, 4, NL_LANE_RELIABLE, 0), NL_SEND_INVALID);
	EXPECT_EQ(nl_send(netlink, 1, 1, &byte, 1, 7, 0), NL_SEND_INVALID);
	EXPECT_EQ(nl_send(netlink, 1, 1, &byte, 1, NL_LANE_RELIABLE, 0), NL_SEND_NOT_RUNNING);

	nl_destroy(netlink);
}


TEST(NetLinkCApi, TwoInstances_DiscoverConnectAndExchangeAMessage)
{
	NLNetLink *a = nl_create();
	NLNetLink *b = nl_create();
	ASSERT_NE(a, nullptr);
	ASSERT_NE(b, nullptr);

	const auto adapters = adaptersOf(a);
	auto	   usable	= std::ranges::find_if(adapters, [](const auto &adapter) { return adapter.priority == NL_ADAPTER_PREFERRED; });

	if (usable == adapters.end())
	{
		nl_destroy(a);
		nl_destroy(b);
		GTEST_SKIP() << "This machine has no preferred network adapter";
	}

	const std::string appId = "netlink-capi-tests-" + std::to_string(std::random_device{}());

	Seen			  seenA;
	Seen			  seenB;
	NLCallbacks		  callbacksA = seenA.callbacks();
	NLCallbacks		  callbacksB = seenB.callbacks();

	NLConfig		  configA{};
	configA.displayName	  = "capi-a";
	configA.appId		  = appId.c_str();
	configA.discoveryPort = DiscoveryPort;

	NLConfig configB	  = configA;
	configB.displayName	  = "capi-b";

	ASSERT_TRUE(nl_set_active_adapter(a, usable->id));
	ASSERT_TRUE(nl_set_active_adapter(b, usable->id));
	ASSERT_TRUE(nl_start(a, &configA, &callbacksA));
	ASSERT_TRUE(nl_start(b, &configB, &callbacksB));
	EXPECT_EQ(nl_get_active_adapter_id(a), usable->id);

	ASSERT_TRUE(waitUntilTrue(
		[&]
		{
			nl_start_discovery(a);
			nl_start_discovery(b);
			return seenA.peerNamed("capi-b").has_value();
		}));

	const uint64_t peerB = *seenA.peerNamed("capi-b");

	NLPeerInfo	   peers[8]{};
	const int32_t  peerCount = nl_get_peers(a, peers, 8);
	EXPECT_TRUE(std::any_of(peers, peers + std::min(peerCount, 8), [&](const NLPeerInfo &peer) { return peer.id == peerB; }));

	ASSERT_TRUE(nl_connect(a, peerB));
	ASSERT_TRUE(waitUntilTrue([&] { return seenA.connectedCount() == 1 && seenB.connectedCount() == 1; }));

	uint64_t connected = 0;
	EXPECT_EQ(nl_get_connected_peers(a, &connected, 1), 1);
	EXPECT_EQ(connected, peerB);

	const std::vector<uint8_t> payload{1, 2, 3, 4, 5};
	ASSERT_EQ(nl_send(a, peerB, 42, payload.data(), payload.size(), NL_LANE_RELIABLE, 0), NL_SEND_QUEUED);
	ASSERT_TRUE(waitUntilTrue([&] { return seenB.messageCount() == 1; }));

	{
		std::lock_guard<std::mutex> lock(seenB.mutex);
		EXPECT_EQ(seenB.messages.front().type, 42u);
		EXPECT_EQ(seenB.messages.front().lane, NL_LANE_RELIABLE);
		EXPECT_EQ(seenB.messages.front().data, payload);
	}

	NLPeerStats stats{};
	EXPECT_TRUE(nl_get_stats(a, peerB, &stats));
	EXPECT_GT(stats.bytesSent, 0u);

	nl_stop(a);
	nl_stop(b);

	{
		std::lock_guard<std::mutex> lock(seenA.mutex);
		EXPECT_FALSE(seenA.logLines.empty()) << "NetLink's log reaches the host";
	}

	nl_destroy(a);
	nl_destroy(b);
}

} // namespace CApiTests
