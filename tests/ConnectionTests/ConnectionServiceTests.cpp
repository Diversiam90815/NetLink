#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <asio.hpp>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ConnectionService/ConnectionService.h"
#include "Signaling/SignalingService.h"
#include "Transport/TransportFactory.h"
#include "Transport/TransportInterfaces.h"

using namespace netlink;
using namespace std::chrono_literals;
using ::testing::_;
using ::testing::Return;


namespace ConnectionTests
{

class MockSession : public ISession
{
public:
	MOCK_METHOD(bool, isConnected, (), (const, override));
	MOCK_METHOD(bool, sendMessage, (netlink::InternalMessage &), (override));
	MOCK_METHOD(void, startReadAsync, (MessageReceivedCallback), (override));
	MOCK_METHOD(void, stopReadAsync, (), (override));
	MOCK_METHOD(int, getBoundPort, (), (const, override));
};


class FakeServer : public IServer
{
public:
	void		   startAccept() override { started = true; }
	int			   getBoundPort() const override { return boundPort; }
	void		   setSessionHandler(SessionHandler handler) override { sessionHandler = std::move(handler); }
	void		   respondToConnectionRequest(bool accepted) override { lastAccepted = accepted; }

	bool		   started{false};
	int			   boundPort{12345};
	bool		   lastAccepted{false};
	SessionHandler sessionHandler;
};


class FakeClient : public IClient
{
public:
	void connect(const std::string &host, unsigned short port) override
	{
		connectedHost = host;
		connectedPort = port;
		connectCalled = true;
	}
	void				  setConnectHandler(ConnectHandler handler) override { connectHandler = std::move(handler); }
	void				  setConnectTimeoutHandler(ConnectTimeoutHandler handler) override { timeoutHandler = std::move(handler); }

	bool				  connectCalled{false};
	std::string			  connectedHost;
	unsigned short		  connectedPort{0};
	ConnectHandler		  connectHandler;
	ConnectTimeoutHandler timeoutHandler;
};


class FakeTransportFactory : public ITransportFactory
{
public:
	std::unique_ptr<IServer> createServer(asio::io_context &) override
	{
		auto server	  = std::make_unique<FakeServer>();
		lastServer	  = server.get();
		serverCreated = true;
		return server;
	}

	std::unique_ptr<IClient> createClient(asio::io_context &) override
	{
		auto client	  = std::make_unique<FakeClient>();
		lastClient	  = client.get();
		clientCreated = true;
		return client;
	}

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


static ValidationResult makeReadyResult(const std::string &name, const std::string &ip = "10.0.0.5", int port = 6000)
{
	ValidationResult r;
	r.remoteEndpoint.displayName = name;
	r.remoteEndpoint.IPAddress	 = ip;
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
		signaling = std::make_unique<SignalingService>(ioContext);
		service	  = std::make_unique<ConnectionService>(ioContext, *signaling, factory);
		service->setLocalIP("10.0.0.1");
	}

	asio::io_context				   ioContext;
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
	EXPECT_EQ(remote->IPAddress, "10.0.0.9");
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

} // namespace ConnectionTests
