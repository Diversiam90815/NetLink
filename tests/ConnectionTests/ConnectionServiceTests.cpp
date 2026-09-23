#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "TestIp.h"
#include "ConnectionService/ConnectionService.h"
#include "Signaling/SignalingService.h"
#include "Transport/TransportFactory.h"
#include "Transport/TransportInterfaces.h"

using namespace netlink;
using namespace std::chrono_literals;


namespace ConnectionTests
{

class FakeSession : public ISession
{
public:
	explicit FakeSession(net::IPv4Address remote) : remoteAddress(remote) {}

	bool			  isConnected() const override { return !closed.load(); }
	bool			  sendMessage(const InternalMessage &, DeliveryMode) override { return isConnected(); }
	void			  startReadAsync(MessageReceivedCallback, DisconnectedCallback) override {}
	void			  stopReadAsync() override {}
	int				  getBoundPort() const override { return 40000; }
	net::IPv4Address  getRemoteAddress() const override { return remoteAddress; }
	int				  getRemotePort() const override { return 50000; }
	void			  close() override { closed.store(true); }

	net::IPv4Address  remoteAddress;
	std::atomic<bool> closed{false};
};


class FakeServer : public IServer
{
public:
	void setSessionHandler(SessionHandler handler) override { sessionHandler = std::move(handler); }
	bool start(const net::IPv4Address &localAddress) override
	{
		startedOn = localAddress;
		started	  = true;
		return startSucceeds;
	}
	void			 stop() override { stopped = true; }
	int				 getBoundPort() const override { return boundPort; }

	bool			 startSucceeds{true};
	bool			 started{false};
	bool			 stopped{false};
	net::IPv4Address startedOn;
	int				 boundPort{12345};
	SessionHandler	 sessionHandler;
};


class FakeClient : public IClient
{
public:
	void connect(const net::IPv4Address &localAddress, const net::IPv4Address &host, unsigned short port) override
	{
		connectedFrom = localAddress;
		connectedHost = host;
		connectedPort = port;
		connectCalled = true;
	}
	void				  setConnectHandler(ConnectHandler handler) override { connectHandler = std::move(handler); }
	void				  setConnectTimeoutHandler(ConnectTimeoutHandler handler) override { timeoutHandler = std::move(handler); }

	bool				  connectCalled{false};
	net::IPv4Address	  connectedFrom;
	net::IPv4Address	  connectedHost;
	unsigned short		  connectedPort{0};
	ConnectHandler		  connectHandler;
	ConnectTimeoutHandler timeoutHandler;
};


class FakeTransportFactory : public ITransportFactory
{
public:
	std::unique_ptr<IServer> createServer() override
	{
		auto server			  = std::make_unique<FakeServer>();
		server->startSucceeds = !failServerStart;
		lastServer			  = server.get();
		serverCreated		  = true;
		return server;
	}

	std::unique_ptr<IClient> createClient() override
	{
		auto client	  = std::make_unique<FakeClient>();
		lastClient	  = client.get();
		clientCreated = true;
		return client;
	}

	bool		failServerStart{false};
	bool		serverCreated{false};
	bool		clientCreated{false};
	FakeServer *lastServer{nullptr};
	FakeClient *lastClient{nullptr};
};


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


static ValidationResult makeReadyResult(const std::string &name, std::string_view ip = "10.0.0.5", int port = 6000)
{
	ValidationResult r;
	r.remoteEndpoint.displayName = name;
	r.remoteEndpoint.IPAddress	 = ipv4(ip);
	r.remoteEndpoint.port		 = port;
	r.status					 = ValidationResult::Status::ReadyToConnect;
	r.canConnect				 = true;
	return r;
}


class ConnectionServiceTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		signaling = std::make_unique<SignalingService>();
		service	  = std::make_unique<ConnectionService>(*signaling, factory);
		service->setLocalIP(ipv4("10.0.0.1"));
	}

	FakeTransportFactory			   factory;
	std::unique_ptr<SignalingService>  signaling;
	std::unique_ptr<ConnectionService> service;
};


// ---------------------------------------------------------------------------
// initiateConnection
// ---------------------------------------------------------------------------

TEST_F(ConnectionServiceTest, InitiateConnection_FailsWithoutValidation)
{
	EXPECT_FALSE(service->initiateConnection("pc-unknown")) << "initiateConnection() must fail when no validation result exists for the peer";
}


TEST_F(ConnectionServiceTest, InitiateConnection_FailsWhenValidationNotReady)
{
	ValidationResult r;
	r.remoteEndpoint.displayName = "pc-a";
	r.canConnect				 = false;
	r.status					 = ValidationResult::Status::VersionMissmatch;
	service->onPeerValidated(r);

	EXPECT_FALSE(service->initiateConnection("pc-a")) << "initiateConnection() must fail when the cached validation result says canConnect == false";
}


TEST_F(ConnectionServiceTest, InitiateConnection_SucceedsWhenValidated)
{
	service->onPeerValidated(makeReadyResult("pc-a"));

	EXPECT_TRUE(service->initiateConnection("pc-a")) << "initiateConnection() must succeed once a ready-to-connect validation result is cached";
	EXPECT_TRUE(service->isConnecting()) << "The service must be in the 'connecting' state right after initiating a connection";
	EXPECT_EQ(service->getConnectionState(), ConnectionStateInternal::InvitationSent) << "After sending the invitation, the internal state must be InvitationSent";
}


TEST_F(ConnectionServiceTest, InitiateConnection_FailsWhenAlreadyConnecting)
{
	service->onPeerValidated(makeReadyResult("pc-a"));
	service->onPeerValidated(makeReadyResult("pc-b"));

	ASSERT_TRUE(service->initiateConnection("pc-a"));
	EXPECT_FALSE(service->initiateConnection("pc-b")) << "A second initiateConnection() call must fail while a connection attempt is already in progress";
}


TEST_F(ConnectionServiceTest, InitiateConnection_SetsCurrentRemote)
{
	service->onPeerValidated(makeReadyResult("pc-a", "10.0.0.9", 7000));
	ASSERT_TRUE(service->initiateConnection("pc-a"));

	auto remote = service->getCurrentRemote();
	ASSERT_TRUE(remote.has_value()) << "getCurrentRemote() must return a value once a connection has been initiated";
	EXPECT_EQ(remote->displayName, "pc-a");
	EXPECT_EQ(remote->IPAddress, ipv4("10.0.0.9"));
}


// ---------------------------------------------------------------------------
// onReceivedInvitation
// ---------------------------------------------------------------------------

TEST_F(ConnectionServiceTest, OnReceivedInvitation_DeclinesWhenPeerNotValidated)
{
	service->onReceivedInvitation("stranger");

	EXPECT_FALSE(service->hasIncomingInvitation()) << "An invitation from an unvalidated peer must be declined and not tracked as pending";
}


TEST_F(ConnectionServiceTest, OnReceivedInvitation_TracksInvitationWhenValidated)
{
	service->onPeerValidated(makeReadyResult("pc-b"));
	service->onReceivedInvitation("pc-b");

	EXPECT_TRUE(service->hasIncomingInvitation()) << "An invitation from a validated peer must be tracked as a pending incoming invitation";
	EXPECT_EQ(service->getConnectionState(), ConnectionStateInternal::InvitationReceived);
}


TEST_F(ConnectionServiceTest, OnReceivedInvitation_AutoAcceptsWhenConfigured)
{
	ConnectionConfig cfg;
	cfg.autoAcceptConnection = true;
	service->setConfig(cfg);
	service->onPeerValidated(makeReadyResult("pc-b"));

	service->onReceivedInvitation("pc-b");

	// auto-accept is posted onto the task queue, so we need to wait for it to run
	EXPECT_TRUE(waitUntil([&] { return service->isConnecting(); })) << "With autoAcceptConnection enabled, the invitation must be automatically accepted asynchronously";
}


// ---------------------------------------------------------------------------
// acceptIncomingConnection / declineIncomingConnection
// ---------------------------------------------------------------------------

TEST_F(ConnectionServiceTest, AcceptIncomingConnection_FailsWithoutPendingInvitation)
{
	EXPECT_FALSE(service->acceptIncomingConnection("pc-b")) << "acceptIncomingConnection() must fail when there is no pending invitation to accept";
}


TEST_F(ConnectionServiceTest, AcceptIncomingConnection_FailsOnNameMismatch)
{
	service->onPeerValidated(makeReadyResult("pc-b"));
	service->onReceivedInvitation("pc-b");

	EXPECT_FALSE(service->acceptIncomingConnection("someone-else"))
		<< "acceptIncomingConnection() must fail if the given computer name doesn't match the pending invitation's remote";
}


TEST_F(ConnectionServiceTest, AcceptIncomingConnection_SucceedsAndEstablishesTransport)
{
	service->onPeerValidated(makeReadyResult("pc-b", "10.0.0.2"));
	service->onReceivedInvitation("pc-b");

	EXPECT_TRUE(service->acceptIncomingConnection("pc-b")) << "acceptIncomingConnection() must succeed for a matching, pending invitation";
	// Local IP 10.0.0.1 vs remote 10.0.0.2 -> remote numerically higher => remote would be Acceptor,
	// meaning the local side (lower IP) becomes the Connector and creates a client.
	EXPECT_TRUE(waitUntil([&] { return factory.clientCreated || factory.serverCreated; }))
		<< "Accepting the invitation must trigger transport role negotiation, creating either a server or a client";
}


TEST_F(ConnectionServiceTest, DeclineIncomingConnection_FailsWithoutPendingInvitation)
{
	EXPECT_FALSE(service->declineIncomingConnection("pc-b")) << "declineIncomingConnection() must fail when there is no pending invitation";
}


TEST_F(ConnectionServiceTest, DeclineIncomingConnection_ClearsPendingInvitation)
{
	service->onPeerValidated(makeReadyResult("pc-b"));
	service->onReceivedInvitation("pc-b");

	EXPECT_TRUE(service->declineIncomingConnection("pc-b", "not now"));
	EXPECT_FALSE(service->hasIncomingInvitation()) << "After declining, the invitation must no longer be tracked as pending";
	EXPECT_EQ(service->getConnectionState(), ConnectionStateInternal::Idle) << "Declining an invitation must reset the connection state back to Idle";
}


// ---------------------------------------------------------------------------
// closeConnection
// ---------------------------------------------------------------------------

TEST_F(ConnectionServiceTest, CloseConnection_FailsWhenNoConnectionExists)
{
	EXPECT_FALSE(service->closeConnection("pc-b")) << "closeConnection() must fail when there is no active or in-progress connection";
}


TEST_F(ConnectionServiceTest, CloseConnection_ClearsInProgressConnection)
{
	service->onPeerValidated(makeReadyResult("pc-a"));
	ASSERT_TRUE(service->initiateConnection("pc-a"));

	EXPECT_TRUE(service->closeConnection("pc-a"));
	EXPECT_EQ(service->getConnectionState(), ConnectionStateInternal::Idle) << "Closing an in-progress connection must reset state back to Idle";
	EXPECT_FALSE(service->isConnecting());
}


// ---------------------------------------------------------------------------
// onReceivedAnswerToInvite
// ---------------------------------------------------------------------------

TEST_F(ConnectionServiceTest, OnReceivedAnswerToInvite_Declined_ClearsConnection)
{
	service->onPeerValidated(makeReadyResult("pc-a"));
	ASSERT_TRUE(service->initiateConnection("pc-a"));

	service->onReceivedAnswerToInvite("pc-a", false, "no thanks");

	EXPECT_EQ(service->getConnectionState(), ConnectionStateInternal::Idle) << "A declined answer must clear the current connection attempt, returning state to Idle";
	EXPECT_FALSE(service->isConnecting());
}


TEST_F(ConnectionServiceTest, OnReceivedAnswerToInvite_Accepted_EstablishesTransport)
{
	service->onPeerValidated(makeReadyResult("pc-a", "10.0.0.9"));
	ASSERT_TRUE(service->initiateConnection("pc-a"));

	service->onReceivedAnswerToInvite("pc-a", true, "");

	EXPECT_TRUE(waitUntil([&] { return factory.clientCreated || factory.serverCreated; }))
		<< "Accepting our invitation must trigger role negotiation and transport creation on the initiator side";
}


TEST_F(ConnectionServiceTest, OnReceivedAnswerToInvite_IgnoredWithoutActiveRequest)
{
	// No initiateConnection() call was made — no current request exists.
	EXPECT_NO_THROW(service->onReceivedAnswerToInvite("pc-a", true, "")) << "Receiving an answer without any active connection request must be safely ignored, not crash";
	EXPECT_EQ(service->getConnectionState(), ConnectionStateInternal::Idle);
}


// ---------------------------------------------------------------------------
// State queries
// ---------------------------------------------------------------------------

TEST_F(ConnectionServiceTest, InitialState_IsIdleAndNotConnected)
{
	EXPECT_EQ(service->getConnectionState(), ConnectionStateInternal::Idle) << "A freshly constructed ConnectionService must start in the Idle state";
	EXPECT_FALSE(service->isConnected());
	EXPECT_FALSE(service->isConnecting());
	EXPECT_FALSE(service->hasIncomingInvitation());
	EXPECT_FALSE(service->getCurrentRemote().has_value()) << "getCurrentRemote() must return nullopt when no connection has been initiated";
}


// ---------------------------------------------------------------------------
// setConfig
// ---------------------------------------------------------------------------

TEST_F(ConnectionServiceTest, SetConfig_DoesNotCrash)
{
	ConnectionConfig cfg;
	cfg.maxConnectionRetries = 5;
	cfg.invitationTimeoutMs	 = 1000;
	EXPECT_NO_THROW(service->setConfig(cfg)) << "Applying a custom config must not throw";
}


// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

TEST_F(ConnectionServiceTest, StatusCallback_FiresOnInitiate)
{
	std::atomic<bool>						  fired{false};
	std::vector<ConnectionStatusUpdate::Type> types;
	std::mutex								  mutex;

	ConnectionServiceCallbacks				  cb;
	cb.onStatusUpdate = [&](const ConnectionStatusUpdate &update)
	{
		std::lock_guard<std::mutex> lock(mutex);
		types.push_back(update.type);
		fired.store(true);
	};
	service->setCallbacks(cb);

	service->onPeerValidated(makeReadyResult("pc-a"));
	service->initiateConnection("pc-a");

	EXPECT_TRUE(fired.load()) << "The onStatusUpdate callback must fire at least once when a connection is initiated";
	std::lock_guard<std::mutex> lock(mutex);
	EXPECT_NE(std::find(types.begin(), types.end(), ConnectionStatusUpdate::Type::Initiated), types.end())
		<< "The Initiated status update must be delivered when initiateConnection() begins";
	EXPECT_NE(std::find(types.begin(), types.end(), ConnectionStatusUpdate::Type::InvitationSent), types.end())
		<< "The InvitationSent status update must be delivered after successfully sending the invitation";
}

// ---------------------------------------------------------------------------
// Transport establishment
// ---------------------------------------------------------------------------

class ConnectionServiceAcceptorTest : public ConnectionServiceTest
{
protected:
	void SetUp() override
	{
		ConnectionServiceTest::SetUp();

		ConnectionServiceCallbacks cb;
		cb.onStatusUpdate = [this](const ConnectionStatusUpdate &update)
		{
			std::lock_guard<std::mutex> lock(updatesMutex);
			updates.push_back(update.type);
		};
		service->setCallbacks(cb);
	}

	// Local 10.0.0.1 is higher than the remote 10.0.0.0 -> local side becomes the Acceptor
	void acceptInvitationAsAcceptor()
	{
		service->onPeerValidated(makeReadyResult("pc-b", "10.0.0.0"));
		service->onReceivedInvitation("pc-b");
		ASSERT_TRUE(service->acceptIncomingConnection("pc-b"));
		ASSERT_NE(factory.lastServer, nullptr) << "The acceptor role must create a server";
	}

	void establish()
	{
		acceptInvitationAsAcceptor();
		factory.lastServer->sessionHandler(std::make_shared<FakeSession>(ipv4("10.0.0.0")));
		ASSERT_TRUE(waitUntil([&] { return service->isConnected(); }));
	}

	bool received(ConnectionStatusUpdate::Type type)
	{
		std::lock_guard<std::mutex> lock(updatesMutex);
		return std::find(updates.begin(), updates.end(), type) != updates.end();
	}

	std::mutex								  updatesMutex;
	std::vector<ConnectionStatusUpdate::Type> updates;
};


TEST_F(ConnectionServiceAcceptorTest, Acceptor_StartsServerOnLocalAddress)
{
	acceptInvitationAsAcceptor();

	EXPECT_TRUE(factory.lastServer->started) << "The acceptor must actually start listening, otherwise it announces port 0";
	EXPECT_EQ(factory.lastServer->startedOn, ipv4("10.0.0.1")) << "The server must listen on the selected adapter address";
}


TEST_F(ConnectionServiceAcceptorTest, Acceptor_FailsWhenServerCannotListen)
{
	factory.failServerStart = true;

	service->onPeerValidated(makeReadyResult("pc-b", "10.0.0.0"));
	service->onReceivedInvitation("pc-b");

	EXPECT_FALSE(service->acceptIncomingConnection("pc-b")) << "A server that cannot listen must fail the connection attempt";
	EXPECT_EQ(service->getConnectionState(), ConnectionStateInternal::Idle);
}


TEST_F(ConnectionServiceAcceptorTest, Acceptor_SessionFromExpectedPeer_EstablishesConnection)
{
	acceptInvitationAsAcceptor();

	auto session = std::make_shared<FakeSession>(ipv4("10.0.0.0"));
	factory.lastServer->sessionHandler(session);

	EXPECT_TRUE(waitUntil([&] { return service->isConnected(); })) << "A session from the negotiated peer must establish the connection";
	EXPECT_TRUE(waitUntil([&] { return received(ConnectionStatusUpdate::Type::Established); }));
	EXPECT_TRUE(factory.lastServer->stopped) << "Once connected, the server must stop accepting further connections";
	EXPECT_FALSE(session->closed.load());
}


TEST_F(ConnectionServiceAcceptorTest, Acceptor_SessionFromUnexpectedAddress_IsRejected)
{
	acceptInvitationAsAcceptor();

	auto intruder = std::make_shared<FakeSession>(ipv4("10.0.0.99"));
	factory.lastServer->sessionHandler(intruder);

	EXPECT_TRUE(waitUntil([&] { return intruder->closed.load(); })) << "A connection from any other host must be closed";
	EXPECT_FALSE(service->isConnected());
}


TEST_F(ConnectionServiceAcceptorTest, TransportDisconnected_ClosesConnection)
{
	establish();

	service->onTransportDisconnected("connection reset");

	EXPECT_TRUE(waitUntil([&] { return !service->isConnected(); })) << "A lost transport must tear down the connection";
	EXPECT_TRUE(waitUntil([&] { return received(ConnectionStatusUpdate::Type::Closed); })) << "A lost transport must be reported as Closed";
	EXPECT_EQ(service->getConnectionState(), ConnectionStateInternal::Idle);
}


TEST_F(ConnectionServiceAcceptorTest, TransportDisconnected_WithoutConnection_IsIgnored)
{
	service->onTransportDisconnected("stale notification");

	std::this_thread::sleep_for(50ms);
	EXPECT_FALSE(received(ConnectionStatusUpdate::Type::Closed));
}


TEST_F(ConnectionServiceAcceptorTest, CloseConnection_WhenConnected_ReportsClosed)
{
	establish();

	EXPECT_TRUE(service->closeConnection("pc-b"));

	EXPECT_FALSE(service->isConnected());
	EXPECT_TRUE(received(ConnectionStatusUpdate::Type::Closing)) << "An established connection must go through the closing path (sends Disconnect to the peer)";
	EXPECT_TRUE(received(ConnectionStatusUpdate::Type::Closed));
}


TEST_F(ConnectionServiceTest, Connector_ConnectsOnceRemoteIsReady)
{
	// Local 10.0.0.1 is lower than the remote 10.0.0.2 -> local side becomes the Connector
	service->onPeerValidated(makeReadyResult("pc-b", "10.0.0.2", 7000));
	service->onReceivedInvitation("pc-b");
	ASSERT_TRUE(service->acceptIncomingConnection("pc-b"));
	ASSERT_NE(factory.lastClient, nullptr) << "The connector role must create a client";

	service->onDataPortReceived("pc-b", 45678);
	service->onReceivedConnectionReadyFlag("pc-b");

	EXPECT_TRUE(factory.lastClient->connectCalled);
	EXPECT_EQ(factory.lastClient->connectedHost, ipv4("10.0.0.2"));
	EXPECT_EQ(factory.lastClient->connectedPort, 45678) << "The connector must use the announced data port, not the signaling port";
	EXPECT_EQ(factory.lastClient->connectedFrom, ipv4("10.0.0.1")) << "The connection must originate from the selected adapter address";

	factory.lastClient->connectHandler(std::make_shared<FakeSession>(ipv4("10.0.0.2")));
	EXPECT_TRUE(waitUntil([&] { return service->isConnected(); }));
}

} // namespace ConnectionTests
