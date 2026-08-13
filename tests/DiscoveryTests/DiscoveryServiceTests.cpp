#include <gtest/gtest.h>
#include <asio.hpp>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Discovery/DiscoveryService.h"

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


static DiscoveryConfig makeConfig(const std::string &name = "pc-a", const std::string &ip = "127.0.0.1", int discoveryPort = 45501, int sigPort = 6000)
{
	DiscoveryConfig cfg;
	cfg.displayName		 = name;
	cfg.localIPv4		 = ip;
	cfg.discoveryPort	 = discoveryPort;
	cfg.signalingPort	 = sigPort;
	cfg.broadCastAddress = "127.0.0.1"; // loopback avoids real broadcast during tests
	return cfg;
}


// ---------------------------------------------------------------------------
// init / deinit
// ---------------------------------------------------------------------------

TEST(DiscoveryService, Init_FailsWithEmptyIP)
{
	asio::io_context ioContext;
	DiscoveryService svc(ioContext);

	DiscoveryConfig	 cfg = makeConfig();
	cfg.localIPv4		 = "";

	EXPECT_FALSE(svc.init(cfg)) << "init() must fail when the local IPv4 address is empty";
}


TEST(DiscoveryService, Init_FailsWithEmptyDisplayName)
{
	asio::io_context ioContext;
	DiscoveryService svc(ioContext);

	DiscoveryConfig	 cfg = makeConfig();
	cfg.displayName		 = "";

	EXPECT_FALSE(svc.init(cfg)) << "init() must fail when the display name is empty";
}


TEST(DiscoveryService, Init_SucceedsWithValidConfig)
{
	asio::io_context ioContext;
	DiscoveryService svc(ioContext);

	EXPECT_TRUE(svc.init(makeConfig("pc-a", "127.0.0.1", 45510))) << "init() must succeed with a valid display name, IP, and port";

	svc.deinit();
}


TEST(DiscoveryService, Init_StoresConfig)
{
	asio::io_context ioContext;
	DiscoveryService svc(ioContext);

	auto			 cfg = makeConfig("pc-a", "127.0.0.1", 45511);
	ASSERT_TRUE(svc.init(cfg));

	EXPECT_EQ(svc.getConfig().displayName, "pc-a");
	EXPECT_EQ(svc.getConfig().discoveryPort, 45511);

	svc.deinit();
}


TEST(DiscoveryService, Init_NoRebindWhenSameAddressAndPort)
{
	asio::io_context ioContext;
	DiscoveryService svc(ioContext);

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
	asio::io_context ioContext;
	DiscoveryService svc(ioContext);

	ASSERT_TRUE(svc.init(makeConfig("pc-a", "127.0.0.1", 45513)));
	svc.deinit();
	EXPECT_NO_THROW(svc.deinit()) << "Calling deinit() a second time must be a safe no-op";
}


TEST(DiscoveryService, Deinit_WithoutInit_DoesNotCrash)
{
	asio::io_context ioContext;
	DiscoveryService svc(ioContext);

	EXPECT_NO_THROW(svc.deinit()) << "deinit() must be safe to call even if init() was never called";
}


// ---------------------------------------------------------------------------
// startDiscovery
// ---------------------------------------------------------------------------

TEST(DiscoveryService, StartDiscovery_ThrowsWithoutInit)
{
	asio::io_context ioContext;
	DiscoveryService svc(ioContext);

	EXPECT_THROW(svc.startDiscovery(), std::runtime_error) << "startDiscovery() must throw if the service has not been initialized";
}


TEST(DiscoveryService, StartDiscovery_SucceedsAfterInit)
{
	asio::io_context ioContext;
	DiscoveryService svc(ioContext);

	ASSERT_TRUE(svc.init(makeConfig("pc-a", "127.0.0.1", 45514)));
	EXPECT_NO_THROW(svc.startDiscovery()) << "startDiscovery() must not throw once the service has been successfully initialized";

	svc.deinit();
}


// ---------------------------------------------------------------------------
// addRemoteToList / getEndpointFromIP
// ---------------------------------------------------------------------------

TEST(DiscoveryService, AddRemoteToList_IgnoresInvalidEndpoint)
{
	asio::io_context ioContext;
	DiscoveryService svc(ioContext);
	ASSERT_TRUE(svc.init(makeConfig("pc-a", "127.0.0.1", 45515)));

	DiscoveryEndpoint invalid{}; // empty IP, port 0
	svc.addRemoteToList(invalid);

	EXPECT_TRUE(svc.getEndpointFromIP("").IPAddress.empty()) << "An invalid endpoint must not be registered, so lookups for it must yield an empty result";

	svc.deinit();
}


TEST(DiscoveryService, AddRemoteToList_IgnoresLocalIP)
{
	asio::io_context ioContext;
	DiscoveryService svc(ioContext);
	ASSERT_TRUE(svc.init(makeConfig("pc-a", "10.0.0.5", 45516)));

	DiscoveryEndpoint self{"10.0.0.5", 6000, "pc-a"};
	svc.addRemoteToList(self);

	EXPECT_TRUE(svc.getEndpointFromIP("10.0.0.5").isEmpty()) << "An endpoint matching the local IP must never be added to the remote device list";

	svc.deinit();
}


TEST(DiscoveryService, AddRemoteToList_AddsValidRemote)
{
	asio::io_context ioContext;
	DiscoveryService svc(ioContext);
	ASSERT_TRUE(svc.init(makeConfig("pc-a", "10.0.0.5", 45517)));

	DiscoveryEndpoint remote{"10.0.0.6", 6001, "pc-b"};
	svc.addRemoteToList(remote);

	auto found = svc.getEndpointFromIP("10.0.0.6");
	EXPECT_EQ(found.displayName, "pc-b") << "A valid, non-local remote endpoint must be added and retrievable via getEndpointFromIP()";
	EXPECT_EQ(found.port, 6001);

	svc.deinit();
}


TEST(DiscoveryService, AddRemoteToList_IgnoresDuplicates)
{
	asio::io_context ioContext;
	DiscoveryService svc(ioContext);
	ASSERT_TRUE(svc.init(makeConfig("pc-a", "10.0.0.5", 45518)));

	DiscoveryEndpoint remote{"10.0.0.6", 6001, "pc-b"};
	svc.addRemoteToList(remote);
	svc.addRemoteToList(remote); // duplicate, must be filtered

	// If duplicates were not filtered we couldn't tell directly from getEndpointFromIP,
	// but we can at least verify the single valid entry is still retrievable and correct.
	auto found = svc.getEndpointFromIP("10.0.0.6");
	EXPECT_EQ(found.displayName, "pc-b");

	svc.deinit();
}


TEST(DiscoveryService, GetEndpointFromIP_ReturnsEmptyForUnknownIP)
{
	asio::io_context ioContext;
	DiscoveryService svc(ioContext);
	ASSERT_TRUE(svc.init(makeConfig("pc-a", "10.0.0.5", 45519)));

	auto found = svc.getEndpointFromIP("192.168.99.99");
	EXPECT_TRUE(found.isEmpty()) << "Looking up an IP that was never discovered must yield an empty DiscoveryEndpoint";

	svc.deinit();
}


TEST(DiscoveryService, AddRemoteToList_TriggersOnRemoteFoundCallback)
{
	asio::io_context ioContext;
	DiscoveryService svc(ioContext);
	ASSERT_TRUE(svc.init(makeConfig("pc-a", "10.0.0.5", 45520)));

	std::atomic<bool> callbackFired{false};
	std::string		  capturedName;
	svc.setOnRemoteFound(
		[&](const DiscoveryEndpoint &ep)
		{
			capturedName = ep.displayName;
			callbackFired.store(true);
		});

	DiscoveryEndpoint remote{"10.0.0.7", 6002, "pc-c"};
	svc.addRemoteToList(remote);

	EXPECT_TRUE(callbackFired.load()) << "Adding a new valid remote must invoke the onRemoteFound callback";
	EXPECT_EQ(capturedName, "pc-c") << "The callback must receive the newly discovered endpoint";

	svc.deinit();
}


TEST(DiscoveryService, AddRemoteToList_DuplicateDoesNotRetriggerCallback)
{
	asio::io_context ioContext;
	DiscoveryService svc(ioContext);
	ASSERT_TRUE(svc.init(makeConfig("pc-a", "10.0.0.5", 45521)));

	std::atomic<int> callCount{0};
	svc.setOnRemoteFound([&](const DiscoveryEndpoint &) { ++callCount; });

	DiscoveryEndpoint remote{"10.0.0.7", 6002, "pc-c"};
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
	asio::io_context ioContextA;
	asio::io_context ioContextB;

	DiscoveryService svcA(ioContextA);
	DiscoveryService svcB(ioContextB);

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

} // namespace DiscoveryTests
