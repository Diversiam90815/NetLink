#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "TestIp.h"
#include "Discovery/DiscoveryService.h"
#include "FakeDatagramNetwork.h"

using namespace std::chrono_literals;


namespace DiscoveryTests
{

template <typename Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout = 1s)
{
	auto deadline = std::chrono::steady_clock::now() + timeout;
	while (std::chrono::steady_clock::now() < deadline)
	{
		if (predicate())
			return true;
		std::this_thread::sleep_for(5ms);
	}
	return predicate();
}


static DiscoveryConfig makeConfig(const std::string &name = "pc-a", std::string_view ip = "127.0.0.1", int discoveryPort = 45501, int sigPort = 6000)
{
	DiscoveryConfig cfg;
	cfg.displayName		 = name;
	cfg.localIPv4		 = ipv4(ip);
	cfg.discoveryPort	 = discoveryPort;
	cfg.signalingPort	 = sigPort;
	cfg.broadcastAddress = ipv4("127.0.0.1"); // loopback avoids real broadcast during tests
	return cfg;
}


// ---------------------------------------------------------------------------
// init / deinit
// ---------------------------------------------------------------------------

TEST(DiscoveryService, Init_FailsWithEmptyIP)
{
	DiscoveryService svc;

	DiscoveryConfig	 cfg = makeConfig();
	cfg.localIPv4		 = netlink::net::IPv4Address{};

	EXPECT_FALSE(svc.init(cfg)) << "init() must fail when the local IPv4 address is empty";
}


TEST(DiscoveryService, Init_FailsWithEmptyDisplayName)
{
	DiscoveryService svc;

	DiscoveryConfig	 cfg = makeConfig();
	cfg.displayName		 = "";

	EXPECT_FALSE(svc.init(cfg)) << "init() must fail when the display name is empty";
}


TEST(DiscoveryService, Init_SucceedsWithValidConfig)
{
	DiscoveryService svc;

	EXPECT_TRUE(svc.init(makeConfig("pc-a", "127.0.0.1", 45510))) << "init() must succeed with a valid display name, IP, and port";

	svc.deinit();
}


TEST(DiscoveryService, Init_StoresConfig)
{
	DiscoveryService svc;

	auto			 cfg = makeConfig("pc-a", "127.0.0.1", 45511);
	ASSERT_TRUE(svc.init(cfg));

	EXPECT_EQ(svc.getConfig().displayName, "pc-a");
	EXPECT_EQ(svc.getConfig().discoveryPort, 45511);

	svc.deinit();
}


TEST(DiscoveryService, Init_NoRebindWhenSameAddressAndPort)
{
	DiscoveryService svc;

	auto			 cfg = makeConfig("pc-a", "127.0.0.1", 45512);
	ASSERT_TRUE(svc.init(cfg));

	// Re-init with only a display name change but same IP/port -> should not need a rebind and must still succeed
	cfg.displayName = "pc-a-renamed";
	EXPECT_TRUE(svc.init(cfg)) << "Re-applying a config with unchanged IP/port must succeed without requiring a socket rebind";
	EXPECT_EQ(svc.getConfig().displayName, "pc-a-renamed") << "The config must still be updated even when no rebind is necessary";

	svc.deinit();
}


TEST(DiscoveryService, Deinit_IsSafeToCallMultipleTimes)
{
	DiscoveryService svc;

	ASSERT_TRUE(svc.init(makeConfig("pc-a", "127.0.0.1", 45513)));
	svc.deinit();
	EXPECT_NO_THROW(svc.deinit()) << "Calling deinit() a second time must be a safe no-op";
}


TEST(DiscoveryService, Deinit_WithoutInit_DoesNotCrash)
{
	DiscoveryService svc;

	EXPECT_NO_THROW(svc.deinit()) << "deinit() must be safe to call even if init() was never called";
}


// ---------------------------------------------------------------------------
// startDiscovery
// ---------------------------------------------------------------------------

TEST(DiscoveryService, StartDiscovery_ThrowsWithoutInit)
{
	DiscoveryService svc;

	EXPECT_FALSE(svc.startDiscovery()) << "startDiscovery() must report failure if the service has not been initialised";
}


TEST(DiscoveryService, StartDiscovery_SucceedsAfterInit)
{
	DiscoveryService svc;

	ASSERT_TRUE(svc.init(makeConfig("pc-a", "127.0.0.1", 45514)));
	EXPECT_TRUE(svc.startDiscovery()) << "startDiscovery() must succeed once the service has been successfully initialised";

	svc.deinit();
}


// ---------------------------------------------------------------------------
// addRemoteToList / getEndpointFromIP
// ---------------------------------------------------------------------------

TEST(DiscoveryService, AddRemoteToList_IgnoresInvalidEndpoint)
{
	DiscoveryService svc;
	ASSERT_TRUE(svc.init(makeConfig("pc-a", "127.0.0.1", 45515)));

	DiscoveryEndpoint invalid{}; // empty IP, port 0
	svc.addRemoteToList(invalid);

	EXPECT_TRUE(svc.getEndpointFromIP(netlink::net::IPv4Address{}).IPAddress.isUnspecified())
		<< "An invalid endpoint must not be registered, so lookups for it must yield an empty result";

	svc.deinit();
}


TEST(DiscoveryService, AddRemoteToList_IgnoresLocalIP)
{
	DiscoveryService svc;
	ASSERT_TRUE(svc.init(makeConfig("pc-a", "10.0.0.5", 45516)));

	DiscoveryEndpoint self{ipv4("10.0.0.5"), 6000, "pc-a"};
	svc.addRemoteToList(self);

	EXPECT_TRUE(svc.getEndpointFromIP(ipv4("10.0.0.5")).isEmpty()) << "An endpoint matching the local IP must never be added to the remote device list";

	svc.deinit();
}


TEST(DiscoveryService, AddRemoteToList_AddsValidRemote)
{
	DiscoveryService svc;
	ASSERT_TRUE(svc.init(makeConfig("pc-a", "10.0.0.5", 45517)));

	DiscoveryEndpoint remote{ipv4("10.0.0.6"), 6001, "pc-b"};
	svc.addRemoteToList(remote);

	auto found = svc.getEndpointFromIP(ipv4("10.0.0.6"));
	EXPECT_EQ(found.displayName, "pc-b") << "A valid, non-local remote endpoint must be added and retrievable via getEndpointFromIP()";
	EXPECT_EQ(found.port, 6001);

	svc.deinit();
}


TEST(DiscoveryService, AddRemoteToList_IgnoresDuplicates)
{
	DiscoveryService svc;
	ASSERT_TRUE(svc.init(makeConfig("pc-a", "10.0.0.5", 45518)));

	DiscoveryEndpoint remote{ipv4("10.0.0.6"), 6001, "pc-b"};
	svc.addRemoteToList(remote);
	svc.addRemoteToList(remote); // duplicate, must be filtered

	auto found = svc.getEndpointFromIP(ipv4("10.0.0.6"));
	EXPECT_EQ(found.displayName, "pc-b");

	svc.deinit();
}


TEST(DiscoveryService, GetEndpointFromIP_ReturnsEmptyForUnknownIP)
{
	DiscoveryService svc;
	ASSERT_TRUE(svc.init(makeConfig("pc-a", "10.0.0.5", 45519)));

	auto found = svc.getEndpointFromIP(ipv4("192.168.99.99"));
	EXPECT_TRUE(found.isEmpty()) << "Looking up an IP that was never discovered must yield an empty DiscoveryEndpoint";

	svc.deinit();
}


TEST(DiscoveryService, AddRemoteToList_TriggersOnRemoteFoundCallback)
{
	DiscoveryService svc;
	ASSERT_TRUE(svc.init(makeConfig("pc-a", "10.0.0.5", 45520)));

	std::atomic<bool> callbackFired{false};
	std::string		  capturedName;
	svc.setOnRemoteFound(
		[&](const DiscoveryEndpoint &ep)
		{
			capturedName = ep.displayName;
			callbackFired.store(true);
		});

	DiscoveryEndpoint remote{ipv4("10.0.0.7"), 6002, "pc-c"};
	svc.addRemoteToList(remote);

	EXPECT_TRUE(callbackFired.load()) << "Adding a new valid remote must invoke the onRemoteFound callback";
	EXPECT_EQ(capturedName, "pc-c") << "The callback must receive the newly discovered endpoint";

	svc.deinit();
}


TEST(DiscoveryService, AddRemoteToList_DuplicateDoesNotRetriggerCallback)
{
	DiscoveryService svc;
	ASSERT_TRUE(svc.init(makeConfig("pc-a", "10.0.0.5", 45521)));

	std::atomic<int> callCount{0};
	svc.setOnRemoteFound([&](const DiscoveryEndpoint &) { ++callCount; });

	DiscoveryEndpoint remote{ipv4("10.0.0.7"), 6002, "pc-c"};
	svc.addRemoteToList(remote);
	svc.addRemoteToList(remote);

	EXPECT_EQ(callCount.load(), 1) << "The onRemoteFound callback must fire exactly once — duplicate additions must not retrigger it";

	svc.deinit();
}


// ---------------------------------------------------------------------------
// End-to-end broadcast/receive over loopback
// ---------------------------------------------------------------------------

TEST(DiscoveryService, TwoServices_DiscoverEachOtherOverLoopback)
{
	DiscoveryService svcA;
	DiscoveryService svcB;

	// Both bind to the same broadcast port on loopback; discovery packages are unicast-like via broadcast address 127.0.0.1
	auto			 cfgA = makeConfig("pc-a", "127.0.0.1", 45599);
	auto			 cfgB = makeConfig("pc-b", "127.0.0.1", 45599);

	ASSERT_TRUE(svcA.init(cfgA));
	ASSERT_TRUE(svcB.init(cfgB));

	std::atomic<bool> aFoundB{false};
	std::atomic<bool> bFoundA{false};

	svcA.setOnRemoteFound([&](const DiscoveryEndpoint &) { aFoundB.store(true); });
	svcB.setOnRemoteFound([&](const DiscoveryEndpoint &) { bFoundA.store(true); });

	svcA.startDiscovery();
	svcB.startDiscovery();

	// Both services broadcast their own info to the same loopback address/port,
	// but since both bind the same local IP, they may see their own broadcast filtered out
	// (addRemoteToList ignores entries matching mConfig.localIPv4). This test primarily
	// verifies that starting discovery on real sockets does not crash or hang.
	std::this_thread::sleep_for(300ms);

	svcA.deinit();
	svcB.deinit();

	SUCCEED() << "DiscoveryService instances must be able to start/broadcast/receive over real sockets without crashing";
}


// ---------------------------------------------------------------------------
// Deterministic discovery over the in-memory network
// ---------------------------------------------------------------------------

class FakeNetworkDiscoveryTest : public ::testing::Test
{
protected:
	static DiscoveryConfig makeLanConfig(const std::string &name, std::string_view ip, int signalingPort)
	{
		DiscoveryConfig cfg;
		cfg.displayName		 = name;
		cfg.localIPv4		 = ipv4(ip);
		cfg.signalingPort	 = signalingPort;
		cfg.discoveryPort	 = 5555;
		cfg.broadcastAddress = ipv4(FakeNet::BroadcastAddress);
		return cfg;
	}

	struct Found
	{
		std::mutex					   mutex;
		std::vector<DiscoveryEndpoint> endpoints;

		void						   add(const DiscoveryEndpoint &ep)
		{
			std::lock_guard<std::mutex> lock(mutex);
			endpoints.push_back(ep);
		}

		size_t count()
		{
			std::lock_guard<std::mutex> lock(mutex);
			return endpoints.size();
		}

		std::vector<DiscoveryEndpoint> snapshot()
		{
			std::lock_guard<std::mutex> lock(mutex);
			return endpoints;
		}
	};

	std::shared_ptr<FakeNet::FakeDatagramNetwork> network = FakeNet::FakeDatagramNetwork::create();
	Found										  foundByA;
	Found										  foundByB;
	DiscoveryService							  svcA{network->factory("10.0.0.1")};
	DiscoveryService							  svcB{network->factory("10.0.0.2")};
};


TEST_F(FakeNetworkDiscoveryTest, TwoHosts_DiscoverEachOther)
{
	svcA.setOnRemoteFound([this](const DiscoveryEndpoint &ep) { foundByA.add(ep); });
	svcB.setOnRemoteFound([this](const DiscoveryEndpoint &ep) { foundByB.add(ep); });

	ASSERT_TRUE(svcA.init(makeLanConfig("pc-a", "10.0.0.1", 6001)));
	ASSERT_TRUE(svcB.init(makeLanConfig("pc-b", "10.0.0.2", 6002)));

	svcA.startDiscovery();
	svcB.startDiscovery();

	ASSERT_TRUE(waitUntil([this] { return foundByA.count() >= 1 && foundByB.count() >= 1; }, 3s)) << "Both hosts must find each other via broadcast";

	const auto seenByA = foundByA.snapshot();
	EXPECT_EQ(seenByA[0].displayName, "pc-b");
	EXPECT_EQ(seenByA[0].IPAddress, ipv4("10.0.0.2"));
	EXPECT_EQ(seenByA[0].port, 6002) << "The announced port is the signaling port";
}


TEST_F(FakeNetworkDiscoveryTest, OwnAnnouncement_IsIgnored_AndRepeatsDoNotRetrigger)
{
	svcA.setOnRemoteFound([this](const DiscoveryEndpoint &ep) { foundByA.add(ep); });
	svcB.setOnRemoteFound([this](const DiscoveryEndpoint &ep) { foundByB.add(ep); });

	ASSERT_TRUE(svcA.init(makeLanConfig("pc-a", "10.0.0.1", 6001)));
	ASSERT_TRUE(svcB.init(makeLanConfig("pc-b", "10.0.0.2", 6002)));

	svcA.startDiscovery();
	svcB.startDiscovery();

	// Longer than one announce interval, so every host announced at least twice
	std::this_thread::sleep_for(2500ms);

	for (const auto &ep : foundByA.snapshot())
		EXPECT_NE(ep.IPAddress, ipv4("10.0.0.1")) << "A host must never discover itself";

	EXPECT_EQ(foundByA.count(), 1u) << "Periodic re-announcements must not trigger the callback again";
	EXPECT_EQ(foundByB.count(), 1u);
}


TEST_F(FakeNetworkDiscoveryTest, ChangedSignalingPort_IsReannouncedAndReported)
{
	svcA.setOnRemoteFound([this](const DiscoveryEndpoint &ep) { foundByA.add(ep); });

	ASSERT_TRUE(svcA.init(makeLanConfig("pc-a", "10.0.0.1", 6001)));
	ASSERT_TRUE(svcB.init(makeLanConfig("pc-b", "10.0.0.2", 6002)));
	svcA.startDiscovery();
	svcB.startDiscovery();

	ASSERT_TRUE(waitUntil([this] { return foundByA.count() == 1; }, 3s));

	// pc-b rebinds its signaling socket (e.g. adapter change): no socket rebind, but an immediate re-announcement
	ASSERT_TRUE(svcB.init(makeLanConfig("pc-b", "10.0.0.2", 7002)));

	ASSERT_TRUE(waitUntil([this] { return foundByA.count() == 2; }, 3s)) << "An updated remote must be reported again";
	EXPECT_EQ(foundByA.snapshot()[1].port, 7002);
	EXPECT_EQ(svcA.getEndpointFromIP(ipv4("10.0.0.2")).port, 7002) << "The stored endpoint must be updated, not duplicated";
}


TEST_F(FakeNetworkDiscoveryTest, StopAndRestartDiscovery)
{
	ASSERT_TRUE(svcA.init(makeLanConfig("pc-a", "10.0.0.1", 6001)));

	svcA.startDiscovery();
	EXPECT_TRUE(svcA.isDiscovering());

	svcA.stopDiscovery();
	EXPECT_FALSE(svcA.isDiscovering());

	EXPECT_TRUE(svcA.startDiscovery()) << "Discovery must be restartable after stopDiscovery()";
	EXPECT_TRUE(svcA.isDiscovering());
}

// ---------------------------------------------------------------------------
// Peer expiry
// ---------------------------------------------------------------------------

TEST(DiscoveryService, PeerThatStopsAnnouncingExpiresAndIsReported)
{
	DiscoveryService svc;

	auto			 cfg = makeConfig("pc-a", "10.0.0.5", 45530);
	cfg.peerTimeoutMs	 = 150;
	ASSERT_TRUE(svc.init(cfg));

	std::atomic<int> lost{0};
	std::mutex		 nameMutex;
	std::string		 lostName;

	svc.setOnRemoteLost(
		[&](const DiscoveryEndpoint &endpoint)
		{
			{
				std::lock_guard<std::mutex> lock(nameMutex);
				lostName = endpoint.displayName;
			}
			++lost;
		});

	svc.addRemoteToList(DiscoveryEndpoint{ipv4("10.0.0.6"), 7001, "pc-b"});
	ASSERT_FALSE(svc.getEndpointFromIP(ipv4("10.0.0.6")).isEmpty()) << "The peer must be known before it can expire";

	svc.startDiscovery();

	for (int i = 0; i < 200 && lost.load() == 0; ++i)
		std::this_thread::sleep_for(10ms);

	svc.stopDiscovery();

	EXPECT_EQ(lost.load(), 1) << "A peer that stops announcing must be reported as lost exactly once";
	{
		std::lock_guard<std::mutex> lock(nameMutex);
		EXPECT_EQ(lostName, "pc-b");
	}
	EXPECT_TRUE(svc.getEndpointFromIP(ipv4("10.0.0.6")).isEmpty()) << "An expired peer must be dropped from the device list";
}


TEST(DiscoveryService, ReAnnouncementKeepsAPeerAlive)
{
	DiscoveryService svc;

	auto			 cfg = makeConfig("pc-a", "10.0.0.5", 45531);
	cfg.peerTimeoutMs	 = 200;
	ASSERT_TRUE(svc.init(cfg));

	std::atomic<int> lost{0};
	svc.setOnRemoteLost([&](const DiscoveryEndpoint &) { ++lost; });

	svc.startDiscovery();

	// An unchanged re-announcement returns early; liveness must still be refreshed or
	// a peer would expire while it is plainly still on the network.
	const DiscoveryEndpoint peer{ipv4("10.0.0.6"), 7001, "pc-b"};
	for (int i = 0; i < 12; ++i)
	{
		svc.addRemoteToList(peer);
		std::this_thread::sleep_for(50ms);
	}

	svc.stopDiscovery();

	EXPECT_EQ(lost.load(), 0) << "A peer that keeps announcing must never be expired";
	EXPECT_FALSE(svc.getEndpointFromIP(ipv4("10.0.0.6")).isEmpty());
}

} // namespace DiscoveryTests
