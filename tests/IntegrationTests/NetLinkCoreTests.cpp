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
#include "FakeDatagramNetwork.h"
#include "LossyDatagramSocket.h"

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


// Two complete NetLink stacks in one process, all traffic (discovery and the peer channel) on an in-memory network
class NetLinkCoreTest : public ::testing::Test
{
protected:
	static constexpr const char *AddressA = "10.0.0.1";
	static constexpr const char *AddressB = "10.0.0.2";

	void						 SetUp() override
	{
		peerA = std::make_unique<NetLinkCore>(dependenciesFor(AddressA));
		peerB = std::make_unique<NetLinkCore>(dependenciesFor(AddressB));
	}

	virtual NetLinkCoreDependencies dependenciesFor(const char *address) { return {cuttable(network->factory(address), address)}; }

	// Cutting a host drops everything it sends and receives, as if its cable was pulled
	net::DatagramSocketFactory		cuttable(net::DatagramSocketFactory inner, const char *address)
	{
		auto flag = std::string(address) == AddressA ? cutA : cutB;

		return [inner = std::move(inner), flag](const net::SocketAddress &local, const net::BindOptions &options) -> net::Result<std::unique_ptr<net::IDatagramSocket>>
		{
			auto socket = inner(local, options);
			if (!socket)
				return std::unexpected(socket.error());
			return std::make_unique<CuttableSocket>(std::move(*socket), flag);
		};
	}

	class CuttableSocket final : public net::IDatagramSocket
	{
	public:
		CuttableSocket(std::unique_ptr<net::IDatagramSocket> inner, std::shared_ptr<std::atomic<bool>> cut) : mInner(std::move(inner)), mCut(std::move(cut)) {}

		net::Result<size_t> sendTo(const net::SocketAddress &destination, std::span<const uint8_t> data) override
		{
			return mCut->load() ? net::Result<size_t>(data.size()) : mInner->sendTo(destination, data);
		}

		net::Result<net::Datagram> receiveFrom(std::span<uint8_t> buffer, std::chrono::milliseconds timeout) override
		{
			auto datagram = mInner->receiveFrom(buffer, timeout);
			if (datagram && mCut->load())
				return std::unexpected(net::SocketError::Timeout);
			return datagram;
		}

		net::SocketAddress localAddress() const override { return mInner->localAddress(); }
		void			   shutdown() override { mInner->shutdown(); }

	private:
		std::unique_ptr<net::IDatagramSocket> mInner;
		std::shared_ptr<std::atomic<bool>>	  mCut;
	};


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

	void start(std::unique_ptr<NetLinkCore> &peer, const NetLinkConfig &config, const NetLinkCallbacks &callbacks, const std::string &address)
	{
		peer->configure(config, callbacks);
		ASSERT_TRUE(peer->init());
		peer->setLocalAddress(address);
	}

	void startDiscovery()
	{
		ASSERT_TRUE(peerA->startDiscovery());
		ASSERT_TRUE(peerB->startDiscovery());
	}

	// Discovers, validates and connects A -> B. B accepts from inside its callback.
	void connectPeers()
	{
		auto callbacksB				   = recordInto(eventsB);
		callbacksB.onConnectionChanged = [this](const ConnectionEvent &event)
		{
			eventsB.addConnectionEvent(event);

			if (event.state == ConnectionState::PendingInbound)
				peerB->respondToConnection(true); // calling back into NetLink from a callback must not deadlock
		};
		callbacksB.onMessageReceived = [this](const Message &message)
		{
			eventsB.addMessage(message);

			switch (reactionB.load())
			{
			case Reaction::Reply: peerB->send(message.type + 1, message.data, DeliveryMode::ReliableOrdered); break;
			case Reaction::Disconnect: peerB->disconnect(); break;
			case Reaction::None: break;
			}
		};

		start(peerA, makeConfig("pc-a", "shared"), recordInto(eventsA), AddressA);
		start(peerB, makeConfig("pc-b", "shared"), callbacksB, AddressB);
		startDiscovery();

		ASSERT_TRUE(waitFor([this] { return !eventsA.discovered().empty() && !eventsB.discovered().empty(); })) << "Both peers must discover and validate each other";

		ASSERT_TRUE(peerA->connectTo(eventsA.discovered().front()));

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
	std::shared_ptr<std::atomic<bool>>			  cutA	  = std::make_shared<std::atomic<bool>>(false);
	std::shared_ptr<std::atomic<bool>>			  cutB	  = std::make_shared<std::atomic<bool>>(false);
	std::shared_ptr<FakeNet::FakeDatagramNetwork> network = FakeNet::FakeDatagramNetwork::create();
	EventRecorder								  eventsA;
	EventRecorder								  eventsB;

	std::unique_ptr<NetLinkCore>				  peerA;
	std::unique_ptr<NetLinkCore>				  peerB;
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

	ASSERT_EQ(peerA->getPotentialEndpoints().size(), 1u);
	EXPECT_EQ(peerA->getPotentialEndpoints().front().displayName, "pc-b");
	EXPECT_EQ(peerA->getConnectionState(), ConnectionState::Searching);

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
	EXPECT_TRUE(peerA->getPotentialEndpoints().empty());
	EXPECT_FALSE(peerA->connectTo({AddressB, 0, "pc-b"})) << "Connecting to an unvalidated peer must be refused";
}


TEST_F(NetLinkCoreTest, FullSession_ConnectExchangeMessagesDisconnect)
{
	connectPeers();

	EXPECT_EQ(peerA->getConnectionState(), ConnectionState::Connected);
	EXPECT_EQ(peerB->getConnectionState(), ConnectionState::Connected);
	EXPECT_EQ(eventsA.lastEventWithState(ConnectionState::Connected)->remote.displayName, "pc-b") << "Connection events must name the remote peer";
	EXPECT_EQ(eventsB.lastEventWithState(ConnectionState::Connected)->remote.displayName, "pc-a");

	// A -> B, and B answers from inside its message callback
	reactionB.store(Reaction::Reply);

	ASSERT_TRUE(peerA->send(10, {1, 2, 3}, DeliveryMode::ReliableOrdered));

	ASSERT_TRUE(waitFor([this] { return !eventsA.messages().empty(); })) << "The reply must arrive at A";

	EXPECT_EQ(eventsB.messages().front().type, 10u);
	EXPECT_EQ(eventsA.messages().front().type, 11u);
	EXPECT_EQ(eventsA.messages().front().data, (std::vector<uint8_t>{1, 2, 3}));

	peerA->disconnect();

	EXPECT_TRUE(waitFor([this] { return eventsA.countState(ConnectionState::Disconnected) == 1 && eventsB.countState(ConnectionState::Disconnected) == 1; }))
		<< "Both peers must report the disconnect";
	EXPECT_FALSE(peerA->send(10, {1}, DeliveryMode::ReliableOrdered)) << "Sending after disconnect must fail";
}


TEST_F(NetLinkCoreTest, Shutdown_NotifiesConnectedRemote)
{
	connectPeers();

	peerA->shutdown();

	EXPECT_TRUE(waitFor([this] { return eventsB.countState(ConnectionState::Disconnected) == 1; })) << "Shutting down must tell the remote instead of leaving it hanging";
	EXPECT_EQ(peerA->getConnectionState(), ConnectionState::None);
}


TEST_F(NetLinkCoreTest, DisconnectFromInsideCallback_DoesNotDeadlock)
{
	connectPeers();

	reactionB.store(Reaction::Disconnect);

	ASSERT_TRUE(peerA->send(1, {42}, DeliveryMode::ReliableOrdered));

	EXPECT_TRUE(waitFor([this] { return eventsA.countState(ConnectionState::Disconnected) == 1; })) << "B's disconnect from within its callback must go through";
}

TEST_F(NetLinkCoreTest, MismatchingApplicationVersion_PeerIsNeverOffered)
{
	start(peerA, makeConfig("pc-a", "shared", "1.0.0"), recordInto(eventsA), AddressA);
	start(peerB, makeConfig("pc-b", "shared", "2.0.0"), recordInto(eventsB), AddressB);
	startDiscovery();

	std::this_thread::sleep_for(1500ms);

	EXPECT_TRUE(eventsA.discovered().empty()) << "A peer running an incompatible application version must not be offered";
	EXPECT_TRUE(peerA->getPotentialEndpoints().empty());
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


TEST_F(NetLinkCoreTest, UnreliableMessages_ReachTheRemote)
{
	connectPeers();

	for (uint32_t i = 0; i < 20; ++i)
		ASSERT_TRUE(peerA->send(100 + i, {static_cast<uint8_t>(i)}, DeliveryMode::UnreliableSequenced));

	EXPECT_TRUE(waitFor([this] { return eventsB.messages().size() == 20; })) << "Without loss every unreliable message arrives";
}


TEST_F(NetLinkCoreTest, LargeMessage_IsDeliveredInOnePiece)
{
	connectPeers();

	std::vector<uint8_t> big(size_t{256} * 1024);
	for (size_t i = 0; i < big.size(); ++i)
		big[i] = static_cast<uint8_t>(i * 3);

	ASSERT_TRUE(peerA->send(5, big, DeliveryMode::ReliableOrdered));

	ASSERT_TRUE(waitFor([this] { return !eventsB.messages().empty(); }, 10s));
	EXPECT_EQ(eventsB.messages().front().data, big);
}


TEST_F(NetLinkCoreTest, VanishedRemote_IsReportedAsDisconnected)
{
	connectPeers();

	// B disappears without telling A (cable pulled): only A's heartbeat supervision notices
	cutB->store(true);

	EXPECT_TRUE(waitFor([this] { return eventsA.countState(ConnectionState::Disconnected) == 1; }, 15s)) << "A lost remote must end the session";
}


// The same stacks on a network that drops, duplicates and reorders datagrams
class LossyNetLinkCoreTest : public NetLinkCoreTest
{
protected:
	NetLinkCoreDependencies dependenciesFor(const char *address) override
	{
		FakeNet::LossProfile	profile{0.2, 0.05, 0.1, address == std::string(AddressA) ? 5u : 9u};

		NetLinkCoreDependencies dependencies;
		dependencies.datagramSocketFactory					  = FakeNet::LossyDatagramSocket::wrap(cuttable(network->factory(address), address), profile);
		dependencies.channelConfig.reliability.maxRetransmits = 20;
		return dependencies;
	}
};


TEST_F(LossyNetLinkCoreTest, FullSession_OverLossyNetwork)
{
	connectPeers();

	const uint32_t total = 500;
	for (uint32_t i = 0; i < total; ++i)
		ASSERT_TRUE(peerA->send(i, {static_cast<uint8_t>(i), static_cast<uint8_t>(i >> 8)}, DeliveryMode::ReliableOrdered));

	ASSERT_TRUE(waitFor([this, total] { return eventsB.messages().size() >= total; }, 30s)) << "Only " << eventsB.messages().size() << " of " << total << " arrived";

	const auto messages = eventsB.messages();
	ASSERT_EQ(messages.size(), total) << "Every message exactly once";
	for (uint32_t i = 0; i < total; ++i)
		ASSERT_EQ(messages[i].type, i) << "In order, broken at " << i;

	peerA->disconnect();
	EXPECT_TRUE(waitFor([this] { return eventsB.countState(ConnectionState::Disconnected) == 1; }, 10s)) << "The Disconnect is delivered reliably as well";
}

} // namespace IntegrationTests
