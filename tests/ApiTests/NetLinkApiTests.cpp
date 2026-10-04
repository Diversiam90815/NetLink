// First: on Windows it brings in WinSock2.h, which has to come before anything that includes windows.h
#include "Socket/Platform/SocketCommon.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "NetLink/NetLink.h"

#include "Network/NetworkInformation.h"
#include "Socket/UdpSocket.h"
#include "Util/ThreadUtils.h"

using namespace netlink;
using namespace std::chrono_literals;


namespace ApiTests
{

// Kept off NetLink's default port, so an application running on this machine is not disturbed
constexpr uint16_t DiscoveryPort = 45461;


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


// What one instance was told through its callbacks
struct Seen
{
	NetLinkCallbacks callbacks()
	{
		NetLinkCallbacks result;
		result.onPeerDiscovered = [this](const PeerInfo &peer)
		{
			std::lock_guard<std::mutex> lock(mutex);
			discovered.push_back(peer);
		};
		result.onConnected = [this](const PeerInfo &peer)
		{
			std::lock_guard<std::mutex> lock(mutex);
			connected.push_back(peer.id);
		};
		result.onDisconnected = [this](const PeerId peer, const DisconnectReason reason)
		{
			std::lock_guard<std::mutex> lock(mutex);
			ended.emplace_back(peer, reason);
		};
		result.onMessage = [this](PeerId, Lane, Message &&message)
		{
			std::lock_guard<std::mutex> lock(mutex);
			messages.push_back(std::move(message));
		};
		return result;
	}

	std::optional<PeerId> peerNamed(const std::string &name)
	{
		std::lock_guard<std::mutex> lock(mutex);
		const auto					it = std::ranges::find_if(discovered, [&](const PeerInfo &peer) { return peer.displayName == name; });
		return it != discovered.end() ? std::optional(it->id) : std::nullopt;
	}

	size_t connectedCount()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return connected.size();
	}

	size_t messageCount()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return messages.size();
	}

	std::vector<std::pair<PeerId, DisconnectReason>> endedSessions()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return ended;
	}

	std::mutex										 mutex;
	std::vector<PeerInfo>							 discovered;
	std::vector<PeerId>								 connected;
	std::vector<std::pair<PeerId, DisconnectReason>> ended;
	std::vector<Message>							 messages;
};


class NetLinkApiTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		NetworkInformation network;
		if (network.init())
			network.processAdapter();

		const auto &adapters = network.getAvailableNetworkAdapters();
		auto		usable = std::ranges::find_if(adapters, [](const auto &candidate) { return candidate.isValid() && candidate.Priority == AdapterPriorityInternal::Preferred; });

		if (usable == adapters.end())
			usable = std::ranges::find_if(adapters, [](const auto &candidate) { return candidate.isValid() && candidate.Priority == AdapterPriorityInternal::Available; });

#if defined(__linux__)
		// Linux delivers broadcasts on the loopback interface as well
		if (usable == adapters.end())
			usable = std::ranges::find_if(adapters, [](const auto &candidate) { return candidate.isValid() && candidate.IPv4.starts_with("127."); });
#endif

		if (usable == adapters.end() || !usable->Eligible)
			GTEST_SKIP() << "This machine has no network adapter NetLink could run on";

		adapter = *usable;

		// Each run is an application of its own: instances of other runs on this network are not in the way
		appId	= "netlink-api-tests-" + std::to_string(std::random_device{}());
	}

	NetLinkConfig configFor(const std::string &name) const
	{
		NetLinkConfig config;
		config.displayName	 = name;
		config.appId		 = appId;
		config.discoveryPort = DiscoveryPort;
		return config;
	}

	bool start(NetLink &instance, const std::string &name, Seen &seen)
	{
		instance.setActiveAdapter(adapter.ID);
		return instance.start(configFor(name), seen.callbacks());
	}

	// Announcing again makes an instance look at its discovery port right away, not only every two seconds
	static std::optional<PeerId> find(NetLink &searching, Seen &seen, NetLink &other, const std::string &name)
	{
		waitUntilTrue(
			[&]
			{
				searching.startDiscovery();
				other.startDiscovery();
				return seen.peerNamed(name).has_value();
			});

		return seen.peerNamed(name);
	}

	NetworkAdapterInternal adapter;
	std::string			   appId;
};


TEST_F(NetLinkApiTest, TwoInstancesOnOneHost_DiscoverAndConnect)
{
	Seen	seenA;
	Seen	seenB;
	NetLink a;
	NetLink b;

	ASSERT_TRUE(start(a, "api-a", seenA));
	ASSERT_TRUE(start(b, "api-b", seenB));

	EXPECT_EQ(a.getActiveAdapterID(), adapter.ID);

	const auto peerB = find(a, seenA, b, "api-b");
	ASSERT_TRUE(peerB.has_value()) << "Two instances on one machine share the discovery port and still find each other";
	ASSERT_TRUE(find(b, seenB, a, "api-a").has_value());

	ASSERT_TRUE(a.connect(*peerB));
	ASSERT_TRUE(waitUntilTrue([&] { return seenA.connectedCount() == 1 && seenB.connectedCount() == 1; }));
	EXPECT_EQ(a.connectedPeers(), std::vector<PeerId>{*peerB});

	const std::vector<uint8_t> payload(100'000, 0x42);
	ASSERT_EQ(a.send(*peerB, 7, payload), SendResult::Queued);
	ASSERT_TRUE(waitUntilTrue([&] { return seenB.messageCount() == 1; }));

	{
		std::lock_guard<std::mutex> lock(seenB.mutex);
		EXPECT_EQ(seenB.messages[0].type, 7u);
		EXPECT_EQ(seenB.messages[0].data, payload);
	}

	ASSERT_TRUE(a.stats(*peerB).has_value());
	EXPECT_EQ(a.stats(*peerB)->bytesSent, payload.size());

	a.stop();
	EXPECT_TRUE(waitUntilTrue([&] { return seenB.endedSessions().size() == 1; }));
	EXPECT_EQ(seenB.endedSessions()[0].second, DisconnectReason::Remote);
}


TEST_F(NetLinkApiTest, ReuseSockets_BothReceiveSubnetBroadcast)
{
	const auto ip	= net::IPv4Address::parse(adapter.IPv4);
	const auto mask = net::IPv4Address::parse(adapter.Subnet);
	ASSERT_TRUE(ip.has_value() && mask.has_value());

	// What the discovery of two engines on one machine relies on
	auto first	= net::UdpSocket::bind(net::SocketAddress::any(DiscoveryPort), {.reuseAddress = true});
	auto second = net::UdpSocket::bind(net::SocketAddress::any(DiscoveryPort), {.reuseAddress = true});
	auto sender = net::UdpSocket::bind({.ip = *ip, .port = 0}, {.enableBroadcast = true});
	ASSERT_TRUE(first.has_value() && second.has_value() && sender.has_value());

	const std::vector<uint8_t> hello{'h', 'e', 'l', 'l', 'o'};
	ASSERT_TRUE(sender->sendTo({.ip = net::subnetBroadcast(*ip, *mask), .port = DiscoveryPort}, hello).has_value());

	std::vector<uint8_t> buffer(64);

	for (auto *socket : {&*first, &*second})
	{
		const auto datagram = socket->receiveFrom(buffer, 2s);
		ASSERT_TRUE(datagram.has_value()) << "A broadcast reaches every socket that shares the port";
		EXPECT_EQ(datagram->size, hello.size());
		EXPECT_EQ(datagram->from, sender->localAddress()) << "... and says where the sender can be reached";
	}
}


TEST_F(NetLinkApiTest, StopFromCallback_ReturnsAndDeliversShutdownEvents)
{
	Seen			  seenA;
	Seen			  seenB;
	NetLink			  a;
	NetLink			  b;
	std::atomic<bool> returned{false};

	auto			  callbacksB = seenB.callbacks();
	callbacksB.onMessage		 = [&](PeerId, Lane, Message &&)
	{
		b.stop();
		returned.store(true);
	};

	ASSERT_TRUE(start(a, "api-a", seenA));
	b.setActiveAdapter(adapter.ID);
	ASSERT_TRUE(b.start(configFor("api-b"), callbacksB));

	const auto peerB = find(a, seenA, b, "api-b");
	ASSERT_TRUE(peerB.has_value());
	ASSERT_TRUE(a.connect(*peerB));
	ASSERT_TRUE(waitUntilTrue([&] { return seenA.connectedCount() == 1 && seenB.connectedCount() == 1; }));

	ASSERT_EQ(a.send(*peerB, 1, std::vector<uint8_t>{1}), SendResult::Queued);

	ASSERT_TRUE(waitUntilTrue([&] { return returned.load(); })) << "stop() from a callback must not wait for the thread it runs on";
	ASSERT_TRUE(waitUntilTrue([&] { return seenB.endedSessions().size() == 1; })) << "The outcome of the session is still delivered";
	EXPECT_EQ(seenB.endedSessions()[0].second, DisconnectReason::Shutdown);

	ASSERT_TRUE(waitUntilTrue([&] { return seenA.endedSessions().size() == 1; }));
	EXPECT_EQ(seenA.endedSessions()[0].second, DisconnectReason::Remote);
	EXPECT_EQ(b.send(PeerId{1}, 1, std::vector<uint8_t>{1}), SendResult::NotRunning);
}


TEST_F(NetLinkApiTest, ThreadCount_IsTwo)
{
	Seen	  seen;
	NetLink	  instance;
	const int before = internal::threadsStarted.load();

	ASSERT_TRUE(start(instance, "api-a", seen));
	ASSERT_TRUE(instance.startDiscovery());
	std::this_thread::sleep_for(300ms);

	EXPECT_EQ(internal::threadsStarted.load() - before, 2) << "One thread for the network, one for the callbacks";

	instance.stop();
	EXPECT_EQ(internal::threadsStarted.load() - before, 2);
}

} // namespace ApiTests
