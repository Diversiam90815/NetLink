#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <optional>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "TestIp.h"
#include "Core/NetLinkCore.h"
#include "Socket/TcpListener.h"
#include "FakeDatagramNetwork.h"

using namespace netlink;
using namespace std::chrono_literals;


namespace IntegrationTests
{

template <typename Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds timeout = 5s)
{
	auto deadline = std::chrono::steady_clock::now() + timeout;
	while (std::chrono::steady_clock::now() < deadline)
	{
		if (predicate())
			return true;
		std::this_thread::sleep_for(10ms);
	}
	return predicate();
}


// Thread-safe record of everything the public callbacks delivered
class EventRecorder
{
public:
	void addDiscovered(const Endpoint &endpoint)
	{
		std::lock_guard<std::mutex> lock(mMutex);
		mDiscovered.push_back(endpoint);
	}

	void addConnectionEvent(const ConnectionEvent &event)
	{
		std::lock_guard<std::mutex> lock(mMutex);
		mConnectionEvents.push_back(event);
	}

	void addMessage(const Message &message)
	{
		std::lock_guard<std::mutex> lock(mMutex);
		mMessages.push_back(message);
	}

	std::vector<Endpoint> discovered()
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return mDiscovered;
	}

	std::vector<Message> messages()
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return mMessages;
	}

	std::optional<ConnectionEvent> lastEventWithState(ConnectionState state)
	{
		std::lock_guard<std::mutex> lock(mMutex);
		auto						it = std::find_if(mConnectionEvents.rbegin(), mConnectionEvents.rend(), [state](const ConnectionEvent &e) { return e.state == state; });
		return it != mConnectionEvents.rend() ? std::optional(*it) : std::nullopt;
	}

	size_t countState(ConnectionState state)
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return static_cast<size_t>(std::count_if(mConnectionEvents.begin(), mConnectionEvents.end(), [state](const ConnectionEvent &e) { return e.state == state; }));
	}

private:
	std::mutex					 mMutex;
	std::vector<Endpoint>		 mDiscovered;
	std::vector<ConnectionEvent> mConnectionEvents;
	std::vector<Message>		 mMessages;
};


// Two complete NetLink stacks in one process: discovery & signaling on an in-memory network,
// the data connection over real TCP on two loopback addresses.
class NetLinkCoreTest : public ::testing::Test
{
protected:
	static constexpr const char *AddressA = "127.0.0.1";
	static constexpr const char *AddressB = "127.0.0.2";

	void						 SetUp() override
	{
		if (!net::TcpListener::listen({ipv4(AddressB), 0}))
			GTEST_SKIP() << AddressB << " is not a usable loopback address on this system (e.g. macOS without an alias)";
	}

	static NetLinkConfig makeConfig(const std::string &name, const std::string &secret, const std::string &version = {})
	{
		NetLinkConfig config;
		config.localDisplayName	  = name;
		config.secret			  = secret;
		config.applicationVersion = version;
		config.discoveryPort	  = 5555;
		config.broadcastAddress	  = FakeNet::BroadcastAddress;
		return config;
	}

	NetLinkCallbacks recordInto(EventRecorder &recorder)
	{
		NetLinkCallbacks callbacks;
		callbacks.onRemoteDiscovered  = [&recorder](const Endpoint &endpoint) { recorder.addDiscovered(endpoint); };
		callbacks.onConnectionChanged = [&recorder](const ConnectionEvent &event) { recorder.addConnectionEvent(event); };
		callbacks.onMessageReceived	  = [&recorder](const Message &message) { recorder.addMessage(message); };
		return callbacks;
	}

	void start(NetLinkCore &peer, const NetLinkConfig &config, const NetLinkCallbacks &callbacks, const std::string &address)
	{
		peer.configure(config, callbacks);
		ASSERT_TRUE(peer.init());
		peer.setLocalAddress(address);
	}

	void startDiscovery()
	{
		ASSERT_TRUE(peerA.startDiscovery());
		ASSERT_TRUE(peerB.startDiscovery());
	}

	// Discovers, validates and connects A -> B. B accepts from inside its callback.
	void connectPeers()
	{
		auto callbacksB				   = recordInto(eventsB);
		callbacksB.onConnectionChanged = [this](const ConnectionEvent &event)
		{
			eventsB.addConnectionEvent(event);

			if (event.state == ConnectionState::PendingInbound)
				peerB.respondToConnection(true); // calling back into NetLink from a callback must not deadlock
		};
		callbacksB.onMessageReceived = [this](const Message &message)
		{
			eventsB.addMessage(message);

			switch (reactionB.load())
			{
			case Reaction::Reply: peerB.send(message.type + 1, message.data, DeliveryMode::ReliableOrdered); break;
			case Reaction::Disconnect: peerB.disconnect(); break;
			case Reaction::None: break;
			}
		};

		start(peerA, makeConfig("pc-a", "shared"), recordInto(eventsA), AddressA);
		start(peerB, makeConfig("pc-b", "shared"), callbacksB, AddressB);
		startDiscovery();

		ASSERT_TRUE(waitFor([this] { return !eventsA.discovered().empty() && !eventsB.discovered().empty(); })) << "Both peers must discover and validate each other";

		ASSERT_TRUE(peerA.connectTo(eventsA.discovered().front()));

		ASSERT_TRUE(waitFor([this] { return eventsA.countState(ConnectionState::Connected) == 1 && eventsB.countState(ConnectionState::Connected) == 1; }))
			<< "Both peers must report an established connection";
	}

	// How peer B reacts to a received message (callbacks are fixed after configure())
	enum class Reaction
	{
		None,
		Reply,
		Disconnect,
	};

	// Declaration order: recorders outlive the peers whose event threads write into them
	std::atomic<Reaction>						  reactionB{Reaction::None};
	std::shared_ptr<FakeNet::FakeDatagramNetwork> network = FakeNet::FakeDatagramNetwork::create();
	EventRecorder								  eventsA;
	EventRecorder								  eventsB;

	NetLinkCore									  peerA{{network->factory(AddressA)}};
	NetLinkCore									  peerB{{network->factory(AddressB)}};
};


TEST_F(NetLinkCoreTest, DiscoveredPeersAreValidatedAndReportedOnce)
{
	start(peerA, makeConfig("pc-a", "shared"), recordInto(eventsA), AddressA);
	start(peerB, makeConfig("pc-b", "shared"), recordInto(eventsB), AddressB);
	startDiscovery();

	ASSERT_TRUE(waitFor([this] { return !eventsA.discovered().empty() && !eventsB.discovered().empty(); }));

	const auto seenByA = eventsA.discovered().front();
	EXPECT_EQ(seenByA.displayName, "pc-b");
	EXPECT_EQ(seenByA.IPAddress, AddressB);

	ASSERT_EQ(peerA.getPotentialEndpoints().size(), 1u);
	EXPECT_EQ(peerA.getPotentialEndpoints().front().displayName, "pc-b");
	EXPECT_EQ(peerA.getConnectionState(), ConnectionState::Searching);

	std::this_thread::sleep_for(2500ms); // at least one more announcement round
	EXPECT_EQ(eventsA.discovered().size(), 1u) << "Re-announcements of an unchanged peer must not report it again";
}


TEST_F(NetLinkCoreTest, MismatchingSecret_PeerIsNeverOffered)
{
	start(peerA, makeConfig("pc-a", "secret-one"), recordInto(eventsA), AddressA);
	start(peerB, makeConfig("pc-b", "secret-two"), recordInto(eventsB), AddressB);
	startDiscovery();

	std::this_thread::sleep_for(1500ms);

	EXPECT_TRUE(eventsA.discovered().empty()) << "An incompatible peer must not be reported";
	EXPECT_TRUE(peerA.getPotentialEndpoints().empty());
	EXPECT_FALSE(peerA.connectTo({AddressB, 0, "pc-b"})) << "Connecting to an unvalidated peer must be refused";
}


TEST_F(NetLinkCoreTest, FullSession_ConnectExchangeMessagesDisconnect)
{
	connectPeers();

	EXPECT_EQ(peerA.getConnectionState(), ConnectionState::Connected);
	EXPECT_EQ(peerB.getConnectionState(), ConnectionState::Connected);
	EXPECT_EQ(eventsA.lastEventWithState(ConnectionState::Connected)->remote.displayName, "pc-b") << "Connection events must name the remote peer";
	EXPECT_EQ(eventsB.lastEventWithState(ConnectionState::Connected)->remote.displayName, "pc-a");

	// A -> B, and B answers from inside its message callback
	reactionB.store(Reaction::Reply);

	ASSERT_TRUE(peerA.send(10, {1, 2, 3}, DeliveryMode::ReliableOrdered));

	ASSERT_TRUE(waitFor([this] { return !eventsA.messages().empty(); })) << "The reply must arrive at A";

	EXPECT_EQ(eventsB.messages().front().type, 10u);
	EXPECT_EQ(eventsA.messages().front().type, 11u);
	EXPECT_EQ(eventsA.messages().front().data, (std::vector<uint8_t>{1, 2, 3}));

	peerA.disconnect();

	EXPECT_TRUE(waitFor([this] { return eventsA.countState(ConnectionState::Disconnected) == 1 && eventsB.countState(ConnectionState::Disconnected) == 1; }))
		<< "Both peers must report the disconnect";
	EXPECT_FALSE(peerA.send(10, {1}, DeliveryMode::ReliableOrdered)) << "Sending after disconnect must fail";
}


TEST_F(NetLinkCoreTest, Shutdown_NotifiesConnectedRemote)
{
	connectPeers();

	peerA.shutdown();

	EXPECT_TRUE(waitFor([this] { return eventsB.countState(ConnectionState::Disconnected) == 1; })) << "Shutting down must tell the remote instead of leaving it hanging";
	EXPECT_EQ(peerA.getConnectionState(), ConnectionState::None);
}


TEST_F(NetLinkCoreTest, DisconnectFromInsideCallback_DoesNotDeadlock)
{
	connectPeers();

	reactionB.store(Reaction::Disconnect);

	ASSERT_TRUE(peerA.send(1, {42}, DeliveryMode::ReliableOrdered));

	EXPECT_TRUE(waitFor([this] { return eventsA.countState(ConnectionState::Disconnected) == 1; })) << "B's disconnect from within its callback must go through";
}

TEST_F(NetLinkCoreTest, MismatchingApplicationVersion_PeerIsNeverOffered)
{
	start(peerA, makeConfig("pc-a", "shared", "1.0.0"), recordInto(eventsA), AddressA);
	start(peerB, makeConfig("pc-b", "shared", "2.0.0"), recordInto(eventsB), AddressB);
	startDiscovery();

	std::this_thread::sleep_for(1500ms);

	EXPECT_TRUE(eventsA.discovered().empty()) << "A peer running an incompatible application version must not be offered";
	EXPECT_TRUE(peerA.getPotentialEndpoints().empty());
}


TEST_F(NetLinkCoreTest, PatchAndBuildNumberDifferencesStayCompatible)
{
	// The build number comes from the commit count, so two builds of the same release
	// must still find each other or no two peers could ever connect.
	start(peerA, makeConfig("pc-a", "shared", "1.4.0.100"), recordInto(eventsA), AddressA);
	start(peerB, makeConfig("pc-b", "shared", "1.4.9.2000"), recordInto(eventsB), AddressB);
	startDiscovery();

	EXPECT_TRUE(waitFor([this] { return !eventsA.discovered().empty() && !eventsB.discovered().empty(); }))
		<< "Builds differing only in patch and build number must stay compatible";
}

} // namespace IntegrationTests
