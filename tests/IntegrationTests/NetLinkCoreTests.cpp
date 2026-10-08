#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Core/NetLinkCore.h"
#include "EngineHarness.h"
#include "FakeDatagramNetwork.h"
#include "LossyDatagramSocket.h"
#include "TestIp.h"

using namespace netlink;
using namespace std::chrono_literals;
using FakeNet::Ended;
using FakeNet::waitUntilTrue;


namespace IntegrationTests
{

// One complete NetLink stack on the in-memory network, with everything its callbacks delivered
struct Stack
{
	Stack(const std::shared_ptr<FakeNet::FakeDatagramNetwork> &network, const std::string &address, net::DatagramSocketFactory factory = {})
		: iface(address),
		  core(NetLinkCoreDependencies{.datagramSocketFactory = factory ? std::move(factory) : cuttable(network, address, cut), .localInterface = iface.provider(), .timings = {}})
	{
	}

	static net::DatagramSocketFactory cuttable(const std::shared_ptr<FakeNet::FakeDatagramNetwork> &network, const std::string &address,
											   const std::shared_ptr<std::atomic<bool>> &cut)
	{
		return FakeNet::CuttableSocket::wrap(network->factory(address), cut);
	}

	// The public callbacks, recorded like the events of an engine
	NetLinkCallbacks callbacks()
	{
		using Kind = EngineEvent::Kind;

		NetLinkCallbacks result;
		result.onPeerDiscovered = [this](const PeerInfo &peer) { record({.kind = Kind::PeerDiscovered, .peer = peer.id, .info = peer}); };
		result.onPeerLost		= [this](const PeerId peer) { record({.kind = Kind::PeerLost, .peer = peer}); };
		result.onConnected		= [this](const PeerInfo &peer) { record({.kind = Kind::Connected, .peer = peer.id, .info = peer}); };
		result.onDisconnected	= [this](const PeerId peer, const DisconnectReason reason) { record({.kind = Kind::Disconnected, .peer = peer, .reason = reason}); };
		result.onMessage		= [this](const PeerId from, const Lane lane, Message &&message)
		{
			if (onMessage)
				onMessage(from, message);

			record({.kind = Kind::Message, .peer = from, .lane = lane, .message = std::move(message)});
		};
		result.onNetworkAdapterChanged = [this](const NetworkAdapter &adapter) { record({.kind = Kind::AdapterChanged, .info = {.address = adapter.ipv4}}); };
		return result;
	}

	void record(EngineEvent event) { events.record(event); }

	bool start(const NetLinkConfig &config, const NetLinkCallbacks &with)
	{
		if (!core.start(config, with))
			return false;

		peerId = core.engine()->id();
		return true;
	}

	bool start(const std::string &name, const std::string &appVersion = {}, const std::string &appId = "core-tests")
	{
		NetLinkConfig config;
		config.displayName = name;
		config.appId	   = appId;
		config.appVersion  = appVersion;
		return start(config, callbacks());
	}

	// Of the latest start
	PeerId													 id() const { return peerId; }

	std::shared_ptr<std::atomic<bool>>						 cut = std::make_shared<std::atomic<bool>>(false);
	FakeNet::TestInterface									 iface;
	FakeNet::EventRecorder									 events;
	std::function<void(PeerId from, const Message &message)> onMessage; // set before start(): what the application does with a message
	PeerId													 peerId;
	NetLinkCore												 core;		// last: its threads are gone before what they report into
};


class NetLinkCoreTest : public ::testing::Test
{
protected:
	static constexpr const char *AddressA = "10.0.0.1";
	static constexpr const char *AddressB = "10.0.0.2";

	void						 SetUp() override
	{
		a = std::make_unique<Stack>(network, AddressA, factoryFor(AddressA));
		b = std::make_unique<Stack>(network, AddressB, factoryFor(AddressB));
	}

	// Empty: the cuttable socket of the stack
	virtual net::DatagramSocketFactory factoryFor(const char *) { return {}; }

	// Both announce themselves until each found the other. Announcing again also makes a core look at its discovery port.
	static bool						   discover(Stack &first, Stack &second, const std::chrono::milliseconds timeout = 5s)
	{
		return waitUntilTrue(
			[&]
			{
				first.core.startDiscovery();
				second.core.startDiscovery();
				return first.events.knows(second.id()) && second.events.knows(first.id());
			},
			timeout);
	}

	static bool connect(Stack &from, Stack &to, const std::chrono::milliseconds timeout = 5s)
	{
		if (!discover(from, to, timeout) || !from.core.connect(to.id()))
			return false;

		return waitUntilTrue([&] { return from.events.isConnectedTo(to.id()) && to.events.isConnectedTo(from.id()); }, timeout);
	}

	void connectPeers()
	{
		ASSERT_TRUE(a->start("pc-a"));
		ASSERT_TRUE(b->start("pc-b"));
		ASSERT_TRUE(connect(*a, *b)) << "Both peers must report an established connection";
	}

	static SendResult send(Stack &from, const Stack &to, const uint32_t type, std::vector<uint8_t> data, const Lane lane = Lane::Reliable)
	{
		return from.core.send(to.id(), type, std::move(data), lane, {});
	}

	std::shared_ptr<FakeNet::FakeDatagramNetwork> network = FakeNet::FakeDatagramNetwork::create();
	std::unique_ptr<Stack>						  a;
	std::unique_ptr<Stack>						  b;
};


TEST_F(NetLinkCoreTest, Start_NeedsAnAppId_AndRunsOnce)
{
	EXPECT_FALSE(a->start("pc-a", {}, "")) << "Without an appId nobody could tell this application from another";
	EXPECT_EQ(a->core.send(PeerId{1}, 1, {}, Lane::Reliable, {}), SendResult::NotRunning);
	EXPECT_FALSE(a->core.startDiscovery());
	EXPECT_FALSE(a->core.connect(PeerId{1}));
	EXPECT_TRUE(a->core.peers().empty());

	ASSERT_TRUE(a->start("pc-a"));
	EXPECT_FALSE(a->start("pc-a")) << "Already running";
	EXPECT_TRUE(a->core.startDiscovery());

	ASSERT_TRUE(waitUntilTrue([this] { return a->events.addresses() == std::vector<std::string>{AddressA}; })) << "The adapter in use is reported";
}


TEST_F(NetLinkCoreTest, DiscoveredPeers_AreReportedOnce)
{
	ASSERT_TRUE(a->start("pc-a"));
	ASSERT_TRUE(b->start("pc-b"));
	ASSERT_TRUE(discover(*a, *b));

	const auto seenByA = a->events.discovered();
	ASSERT_EQ(seenByA.size(), 1u);
	EXPECT_EQ(seenByA[0].displayName, "pc-b");
	EXPECT_EQ(seenByA[0].address, AddressB);
	EXPECT_EQ(seenByA[0].id, b->id());

	ASSERT_EQ(a->core.peers().size(), 1u);
	EXPECT_EQ(a->core.peers().front().displayName, "pc-b");

	std::this_thread::sleep_for(2500ms); // at least one more announcement round
	EXPECT_EQ(a->events.discovered().size(), 1u) << "Re-announcements of an unchanged peer must not report it again";
}


TEST_F(NetLinkCoreTest, OtherApplication_IsNeverOffered)
{
	ASSERT_TRUE(a->start("pc-a", {}, "app-one"));
	ASSERT_TRUE(b->start("pc-b", {}, "app-two"));

	EXPECT_FALSE(discover(*a, *b, 1500ms)) << "Only peers of the same application see each other";
	EXPECT_TRUE(a->core.peers().empty());
	EXPECT_FALSE(a->core.connect(b->id())) << "Connecting to a peer that was not discovered must be refused";
}


TEST_F(NetLinkCoreTest, MismatchingApplicationVersion_PeerIsNeverOffered)
{
	ASSERT_TRUE(a->start("pc-a", "1.0.0"));
	ASSERT_TRUE(b->start("pc-b", "2.0.0"));

	EXPECT_FALSE(discover(*a, *b, 1500ms)) << "A peer running an incompatible application version must not be offered";
	EXPECT_TRUE(a->core.peers().empty());
}


TEST_F(NetLinkCoreTest, PatchAndBuildNumberDifferencesStayCompatible)
{
	// The build number comes from the commit count, so two builds of the same release
	// must still find each other or no two peers could ever connect.
	ASSERT_TRUE(a->start("pc-a", "1.4.0.100"));
	ASSERT_TRUE(b->start("pc-b", "1.4.9.2000"));

	EXPECT_TRUE(discover(*a, *b)) << "Builds differing only in patch and build number must stay compatible";
	EXPECT_EQ(a->events.discovered()[0].appVersion, "1.4");
}


TEST_F(NetLinkCoreTest, FullSession_ConnectExchangeMessagesDisconnect)
{
	// B answers from inside its message callback: calling back into NetLink from a callback must not deadlock
	b->onMessage = [this](const PeerId from, const Message &message) { b->core.send(from, message.type + 1, std::vector<uint8_t>(message.data), Lane::Reliable, {}); };

	connectPeers();

	EXPECT_EQ(a->events.connected()[0].displayName, "pc-b") << "The connection names the remote peer";
	EXPECT_EQ(b->events.connected()[0].displayName, "pc-a");
	EXPECT_EQ(a->core.connectedPeers(), std::vector<PeerId>{b->id()});

	ASSERT_EQ(send(*a, *b, 10, {1, 2, 3}), SendResult::Queued);

	ASSERT_TRUE(waitUntilTrue([this] { return a->events.messageCount() == 1; })) << "The reply must arrive at A";

	EXPECT_EQ(b->events.messages().front().type, 10u);
	EXPECT_EQ(b->events.messages().front().from, a->id());
	EXPECT_EQ(a->events.messages().front().type, 11u);
	EXPECT_EQ(a->events.messages().front().data, (std::vector<uint8_t>{1, 2, 3}));

	a->core.disconnect(b->id());

	EXPECT_TRUE(waitUntilTrue([this] { return !a->events.ended().empty() && !b->events.ended().empty(); })) << "Both peers must report the disconnect";
	EXPECT_EQ(a->events.ended(), std::vector<Ended>({{b->id(), DisconnectReason::Local}}));
	EXPECT_EQ(b->events.ended(), std::vector<Ended>({{a->id(), DisconnectReason::Remote}}));
	EXPECT_EQ(send(*a, *b, 10, {1}), SendResult::NotConnected) << "Sending after disconnect must fail";
}


TEST_F(NetLinkCoreTest, ConnectionRequest_IsAnsweredFromItsCallback)
{
	auto callbacksB				   = b->callbacks();
	callbacksB.onConnectionRequest = [this](const PeerInfo &peer)
	{
		b->record({.kind = EngineEvent::Kind::ConnectionRequest, .peer = peer.id, .info = peer});
		b->core.accept(peer.id);
	};

	NetLinkConfig configB;
	configB.displayName = "pc-b";
	configB.appId		= "core-tests";

	ASSERT_TRUE(a->start("pc-a"));
	ASSERT_TRUE(b->start(configB, callbacksB));
	ASSERT_TRUE(connect(*a, *b));

	ASSERT_EQ(b->events.requests().size(), 1u);
	EXPECT_EQ(b->events.requests()[0].displayName, "pc-a");

	const auto order = b->events.order();
	const auto asked = std::ranges::find(order, std::pair{EngineEvent::Kind::ConnectionRequest, a->id()});
	const auto done	 = std::ranges::find(order, std::pair{EngineEvent::Kind::Connected, a->id()});
	EXPECT_LT(asked, done);
}


TEST_F(NetLinkCoreTest, ThreeStacks_HoldSessionsWithEachOther)
{
	Stack c(network, "10.0.0.3");

	ASSERT_TRUE(a->start("pc-a"));
	ASSERT_TRUE(b->start("pc-b"));
	ASSERT_TRUE(c.start("pc-c"));

	ASSERT_TRUE(connect(*a, *b));
	ASSERT_TRUE(connect(*a, c));
	ASSERT_TRUE(connect(*b, c));

	EXPECT_EQ(a->core.connectedPeers().size(), 2u);
	EXPECT_EQ(a->core.broadcast(7, std::vector<uint8_t>{9}, Lane::Reliable), 2u);
	ASSERT_EQ(send(c, *a, 8, {1}), SendResult::Queued);

	ASSERT_TRUE(waitUntilTrue([&] { return b->events.messageCount() == 1 && c.events.messageCount() == 1 && a->events.messageCount() == 1; }));
	EXPECT_EQ(a->events.messages()[0].from, c.id()) << "Every message says whom it is from";
	EXPECT_EQ(c.events.messages()[0].from, a->id());

	c.core.stop();
}


TEST_F(NetLinkCoreTest, Stop_NotifiesConnectedRemote)
{
	connectPeers();

	a->core.stop();

	EXPECT_EQ(a->events.ended(), std::vector<Ended>({{b->id(), DisconnectReason::Shutdown}})) << "The last event is delivered before stop() returns";
	EXPECT_TRUE(waitUntilTrue([this] { return !b->events.ended().empty(); })) << "Stopping must tell the remote instead of leaving it hanging";
	EXPECT_EQ(b->events.ended(), std::vector<Ended>({{a->id(), DisconnectReason::Remote}}));
	EXPECT_EQ(a->core.send(PeerId{1}, 1, {}, Lane::Reliable, {}), SendResult::NotRunning);

	EXPECT_NO_THROW(a->core.stop());
}


TEST_F(NetLinkCoreTest, StopAndStartAgain_IsANewPeer)
{
	connectPeers();
	const PeerId before = a->id();

	a->core.stop();
	ASSERT_TRUE(a->start("pc-a"));

	EXPECT_NE(a->id(), before);
	ASSERT_TRUE(discover(*a, *b)) << "For B it is a peer it has not seen before";

	ASSERT_TRUE(b->core.connect(a->id()));
	EXPECT_TRUE(waitUntilTrue([this] { return b->events.connected().size() == 2; }));
}


TEST_F(NetLinkCoreTest, DisconnectFromInsideCallback_DoesNotDeadlock)
{
	b->onMessage = [this](const PeerId from, const Message &) { b->core.disconnect(from); };

	connectPeers();
	ASSERT_EQ(send(*a, *b, 1, {42}), SendResult::Queued);

	EXPECT_TRUE(waitUntilTrue([this] { return !a->events.ended().empty(); })) << "B's disconnect from within its callback must go through";
	EXPECT_EQ(a->events.ended(), std::vector<Ended>({{b->id(), DisconnectReason::Remote}}));
}


TEST_F(NetLinkCoreTest, StopFromInsideCallback_ReturnsAndDeliversTheLastEvents)
{
	std::atomic<bool> returned{false};

	b->onMessage = [&](PeerId, const Message &)
	{
		b->core.stop();
		returned.store(true);
	};

	connectPeers();
	ASSERT_EQ(send(*a, *b, 1, {42}), SendResult::Queued);

	ASSERT_TRUE(waitUntilTrue([&] { return returned.load(); })) << "A callback cannot wait for its own thread";
	EXPECT_TRUE(waitUntilTrue([this] { return b->events.ended() == std::vector<Ended>({{a->id(), DisconnectReason::Shutdown}}); })) << "The session's outcome still arrives";
	EXPECT_TRUE(waitUntilTrue([this] { return a->events.ended() == std::vector<Ended>({{b->id(), DisconnectReason::Remote}}); }));

	// Whoever stops again from elsewhere cleans up; after that the stack starts again
	b->core.stop();
	EXPECT_TRUE(b->start("pc-b"));
}


TEST_F(NetLinkCoreTest, MediaMessages_ReachTheRemote)
{
	connectPeers();

	for (uint32_t i = 0; i < 20; ++i)
		ASSERT_EQ(send(*a, *b, 100 + i, {static_cast<uint8_t>(i)}, Lane::Media), SendResult::Queued);

	EXPECT_TRUE(waitUntilTrue([this] { return b->events.messageCount() == 20; })) << "Without loss every Media message arrives";
	EXPECT_EQ(b->events.messages()[0].lane, Lane::Media);
}


TEST_F(NetLinkCoreTest, LargeMessage_IsDeliveredInOnePiece)
{
	connectPeers();

	std::vector<uint8_t> big(size_t{256} * 1024);
	for (size_t i = 0; i < big.size(); ++i)
		big[i] = static_cast<uint8_t>(i * 3);

	ASSERT_EQ(send(*a, *b, 5, big, Lane::Bulk), SendResult::Queued);

	ASSERT_TRUE(waitUntilTrue([this] { return b->events.messageCount() == 1; }, 10s));
	EXPECT_EQ(b->events.messages().front().data, big);
	EXPECT_EQ(a->core.stats(b->id())->bytesSent, big.size());
}


TEST_F(NetLinkCoreTest, VanishedRemote_IsReportedAsLost)
{
	NetLinkConfig config;
	config.displayName = "pc-a";
	config.appId	   = "core-tests";
	config.peerTimeout = 1500ms;

	ASSERT_TRUE(a->start(config, a->callbacks()));
	ASSERT_TRUE(b->start("pc-b"));
	ASSERT_TRUE(connect(*a, *b));

	// B disappears without telling A (cable pulled): A notices because B does not answer anymore
	b->cut->store(true);

	EXPECT_TRUE(waitUntilTrue([this] { return !a->events.ended().empty(); }, 10s)) << "A lost remote must end the session";
	EXPECT_EQ(a->events.ended(), std::vector<Ended>({{b->id(), DisconnectReason::Lost}}));
}


TEST_F(NetLinkCoreTest, Log_IsDeliveredOnTheEventThread)
{
	std::mutex				  mutex;
	std::vector<std::string>  lines;
	std::set<std::thread::id> threads;
	std::thread::id			  callbackThread;

	auto					  callbacks = a->callbacks();
	callbacks.onConnected				= [&](const PeerInfo &)
	{
		std::lock_guard<std::mutex> lock(mutex);
		callbackThread = std::this_thread::get_id();
	};
	callbacks.onLog = [&](LogLevel, const std::string_view message)
	{
		std::lock_guard<std::mutex> lock(mutex);
		lines.emplace_back(message);
		threads.insert(std::this_thread::get_id());
	};

	NetLinkConfig config;
	config.displayName = "pc-a";
	config.appId	   = "core-tests";

	ASSERT_TRUE(a->start(config, callbacks));
	ASSERT_TRUE(b->start("pc-b"));
	ASSERT_TRUE(waitUntilTrue(
		[&]
		{
			a->core.startDiscovery();
			b->core.startDiscovery();
			return b->events.knows(a->id()) && !a->core.peers().empty();
		}));
	ASSERT_TRUE(a->core.connect(b->id()));
	ASSERT_TRUE(waitUntilTrue([&] { return b->events.isConnectedTo(a->id()); }));

	const auto contains = [&](const std::string_view text)
	{
		std::lock_guard<std::mutex> lock(mutex);
		return std::ranges::any_of(lines, [&](const std::string &line) { return line.find(text) != std::string::npos; });
	};

	EXPECT_TRUE(waitUntilTrue([&] { return contains("starting as 'pc-a'"); })) << "What start() says on the caller's thread";
	EXPECT_TRUE(waitUntilTrue([&] { return contains("Engine bound to 10.0.0.1"); })) << "What the I/O thread says";

	std::lock_guard<std::mutex> lock(mutex);
	ASSERT_EQ(threads.size(), 1u) << "The application reads every line on one thread";
	EXPECT_EQ(*threads.begin(), callbackThread) << "... the one its other callbacks run on";
	EXPECT_FALSE(threads.contains(std::this_thread::get_id()));
}


// The same stacks on a network that drops, duplicates and reorders datagrams
class LossyNetLinkCoreTest : public NetLinkCoreTest
{
protected:
	net::DatagramSocketFactory factoryFor(const char *address) override
	{
		const FakeNet::LossProfile profile{0.2, 0.05, 0.1, address == std::string(AddressA) ? 5u : 9u};
		return FakeNet::LossyDatagramSocket::wrap(network->factory(address), profile);
	}
};


TEST_F(LossyNetLinkCoreTest, FullSession_OverLossyNetwork)
{
	NetLinkConfig config;
	config.appId	   = "core-tests";
	config.peerTimeout = 20s;

	config.displayName = "pc-a";
	ASSERT_TRUE(a->start(config, a->callbacks()));
	config.displayName = "pc-b";
	ASSERT_TRUE(b->start(config, b->callbacks()));
	ASSERT_TRUE(connect(*a, *b, 15s));

	const uint32_t total = 500;
	for (uint32_t i = 0; i < total; ++i)
		ASSERT_EQ(send(*a, *b, i, {static_cast<uint8_t>(i), static_cast<uint8_t>(i >> 8)}), SendResult::Queued);

	ASSERT_TRUE(waitUntilTrue([this, total] { return b->events.messageCount() >= total; }, 30s)) << "Only " << b->events.messageCount() << " of " << total << " arrived";

	const auto messages = b->events.messages();
	ASSERT_EQ(messages.size(), total) << "Every message exactly once";
	for (uint32_t i = 0; i < total; ++i)
		ASSERT_EQ(messages[i].type, i) << "In order, broken at " << i;

	a->core.disconnect(b->id());
	EXPECT_TRUE(waitUntilTrue([this] { return !a->events.ended().empty(); }, 10s));
}

} // namespace IntegrationTests
