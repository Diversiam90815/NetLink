#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "TestIp.h"
#include "Channel/PeerChannel.h"
#include "ConnectionService/ConnectionService.h"
#include "FakeDatagramNetwork.h"

using namespace netlink;
using namespace std::chrono_literals;


namespace ConnectionTests
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


// The service talks through a real, bound PeerChannel; its packets go into an in-memory network nobody listens on.
// Signals from the remote are injected by calling the service's handlers directly.
class ConnectionServiceTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		ASSERT_TRUE(channel.init("pc-local"));
		channel.setLocalIPv4(ipv4("10.0.0.1"));

		service = std::make_unique<ConnectionService>(channel);
		service->setLocalIP(ipv4("10.0.0.1"));

		ConnectionServiceCallbacks cb;
		cb.onStatusUpdate = [this](const ConnectionStatusUpdate &update)
		{
			std::lock_guard<std::mutex> lock(updatesMutex);
			updates.push_back(update.type);
			lastMessage = update.message;
		};
		service->setCallbacks(cb);
	}

	void TearDown() override
	{
		service.reset();
		channel.deinit();
	}

	// Discovered (known to the channel) and validated
	void validate(const std::string &name, std::string_view ip = "10.0.0.5", int port = 6000)
	{
		channel.registerPeer(name, ipv4(ip), port);
		service->onPeerValidated(makeReadyResult(name, ip, port));
	}

	bool received(ConnectionStatusUpdate::Type type)
	{
		std::lock_guard<std::mutex> lock(updatesMutex);
		return std::find(updates.begin(), updates.end(), type) != updates.end();
	}

	std::string message()
	{
		std::lock_guard<std::mutex> lock(updatesMutex);
		return lastMessage;
	}

	// Initiator side: invitation sent and accepted, waiting for the remote's ready flag
	void acceptedAsInitiator(const std::string &name = "pc-a")
	{
		validate(name);
		ASSERT_TRUE(service->initiateConnection(name));
		service->onReceivedAnswerToInvite(name, true, "");
		ASSERT_EQ(service->getConnectionState(), ConnectionStateInternal::AwaitingReadyFlag);
	}

	// Acceptor side: invitation received and accepted, waiting for the remote's ready flag
	void acceptedAsAcceptor(const std::string &name = "pc-b")
	{
		validate(name);
		service->onReceivedInvitation(name);
		ASSERT_TRUE(service->acceptIncomingConnection(name));
		ASSERT_EQ(service->getConnectionState(), ConnectionStateInternal::AwaitingReadyFlag);
	}

	void establish(const std::string &name = "pc-b")
	{
		acceptedAsAcceptor(name);
		service->onReadyFlagReceived(name);
		ASSERT_TRUE(waitUntil([&] { return received(ConnectionStatusUpdate::Type::Established); }));
	}

	std::shared_ptr<FakeNet::FakeDatagramNetwork> network = FakeNet::FakeDatagramNetwork::create();
	PeerChannel									  channel{network->factory("10.0.0.1")};
	std::unique_ptr<ConnectionService>			  service;

	std::mutex									  updatesMutex;
	std::vector<ConnectionStatusUpdate::Type>	  updates;
	std::string									  lastMessage;
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
	validate("pc-a");

	EXPECT_TRUE(service->initiateConnection("pc-a")) << "initiateConnection() must succeed once a ready-to-connect validation result is cached";
	EXPECT_TRUE(service->isConnecting()) << "The service must be in the 'connecting' state right after initiating a connection";
	EXPECT_EQ(service->getConnectionState(), ConnectionStateInternal::InvitationSent) << "After sending the invitation, the internal state must be InvitationSent";
}


TEST_F(ConnectionServiceTest, InitiateConnection_FailsWhenTheInvitationCannotBeSent)
{
	// Validated, but the channel does not know where the peer lives
	service->onPeerValidated(makeReadyResult("pc-a"));

	EXPECT_FALSE(service->initiateConnection("pc-a"));
	EXPECT_EQ(service->getConnectionState(), ConnectionStateInternal::Idle);
	EXPECT_TRUE(received(ConnectionStatusUpdate::Type::Failed));
}


TEST_F(ConnectionServiceTest, InitiateConnection_FailsWhenAlreadyConnecting)
{
	validate("pc-a");
	validate("pc-b", "10.0.0.6");

	ASSERT_TRUE(service->initiateConnection("pc-a"));
	EXPECT_FALSE(service->initiateConnection("pc-b")) << "A second initiateConnection() call must fail while a connection attempt is already in progress";
}


TEST_F(ConnectionServiceTest, InitiateConnection_SetsCurrentRemote)
{
	validate("pc-a", "10.0.0.9", 7000);
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
	validate("pc-b");
	service->onReceivedInvitation("pc-b");

	EXPECT_TRUE(service->hasIncomingInvitation()) << "An invitation from a validated peer must be tracked as a pending incoming invitation";
	EXPECT_EQ(service->getConnectionState(), ConnectionStateInternal::InvitationReceived);
}


TEST_F(ConnectionServiceTest, OnReceivedInvitation_AutoAcceptsWhenConfigured)
{
	ConnectionConfig cfg;
	cfg.autoAcceptConnection = true;
	service->setConfig(cfg);
	validate("pc-b");

	service->onReceivedInvitation("pc-b");

	// auto-accept is posted onto the task queue, so we need to wait for it to run
	EXPECT_TRUE(waitUntil([&] { return service->getConnectionState() == ConnectionStateInternal::AwaitingReadyFlag; }))
		<< "With autoAcceptConnection enabled, the invitation must be automatically accepted asynchronously";
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
	validate("pc-b");
	service->onReceivedInvitation("pc-b");

	EXPECT_FALSE(service->acceptIncomingConnection("someone-else"))
		<< "acceptIncomingConnection() must fail if the given computer name doesn't match the pending invitation's remote";
}


TEST_F(ConnectionServiceTest, AcceptIncomingConnection_OpensTheSession)
{
	acceptedAsAcceptor();

	EXPECT_TRUE(received(ConnectionStatusUpdate::Type::Accepted));
	EXPECT_TRUE(received(ConnectionStatusUpdate::Type::Establishing));
	EXPECT_FALSE(service->isConnected()) << "Connected only once the remote's ready flag arrived";
	EXPECT_TRUE(service->isConnecting());
}


TEST_F(ConnectionServiceTest, DeclineIncomingConnection_FailsWithoutPendingInvitation)
{
	EXPECT_FALSE(service->declineIncomingConnection("pc-b")) << "declineIncomingConnection() must fail when there is no pending invitation";
}


TEST_F(ConnectionServiceTest, DeclineIncomingConnection_ClearsPendingInvitation)
{
	validate("pc-b");
	service->onReceivedInvitation("pc-b");

	EXPECT_TRUE(service->declineIncomingConnection("pc-b", "not now"));
	EXPECT_FALSE(service->hasIncomingInvitation()) << "After declining, the invitation must no longer be tracked as pending";
	EXPECT_EQ(service->getConnectionState(), ConnectionStateInternal::Idle) << "Declining an invitation must reset the connection state back to Idle";
}


// ---------------------------------------------------------------------------
// onReceivedAnswerToInvite
// ---------------------------------------------------------------------------

TEST_F(ConnectionServiceTest, OnReceivedAnswerToInvite_Declined_ClearsConnection)
{
	validate("pc-a");
	ASSERT_TRUE(service->initiateConnection("pc-a"));

	service->onReceivedAnswerToInvite("pc-a", false, "no thanks");

	EXPECT_EQ(service->getConnectionState(), ConnectionStateInternal::Idle) << "A declined answer must clear the current connection attempt, returning state to Idle";
	EXPECT_FALSE(service->isConnecting());
	EXPECT_NE(message().find("no thanks"), std::string::npos) << "The remote's reason must reach the app";
}


TEST_F(ConnectionServiceTest, OnReceivedAnswerToInvite_Accepted_OpensTheSession)
{
	acceptedAsInitiator();

	EXPECT_TRUE(received(ConnectionStatusUpdate::Type::Accepted));
	EXPECT_TRUE(received(ConnectionStatusUpdate::Type::Establishing));
}


TEST_F(ConnectionServiceTest, OnReceivedAnswerToInvite_IgnoredWithoutActiveRequest)
{
	// No initiateConnection() call was made — no current request exists.
	EXPECT_NO_THROW(service->onReceivedAnswerToInvite("pc-a", true, "")) << "Receiving an answer without any active connection request must be safely ignored, not crash";
	EXPECT_EQ(service->getConnectionState(), ConnectionStateInternal::Idle);
}


// ---------------------------------------------------------------------------
// Session establishment (ready flags)
// ---------------------------------------------------------------------------

TEST_F(ConnectionServiceTest, Acceptor_ConnectsOnceTheRemoteIsReady)
{
	acceptedAsAcceptor();

	service->onReadyFlagReceived("pc-b");

	EXPECT_TRUE(waitUntil([&] { return received(ConnectionStatusUpdate::Type::Established); }));
	EXPECT_TRUE(service->isConnected());
	EXPECT_FALSE(service->isConnecting());
	EXPECT_EQ(service->getConnectionState(), ConnectionStateInternal::Connected);
}


TEST_F(ConnectionServiceTest, Initiator_ConnectsOnceTheRemoteIsReady)
{
	acceptedAsInitiator();

	service->onReadyFlagReceived("pc-a");

	EXPECT_TRUE(waitUntil([&] { return received(ConnectionStatusUpdate::Type::Established); }));
	EXPECT_TRUE(service->isConnected());
}


TEST_F(ConnectionServiceTest, EarlyReadyFlag_CompletesAsSoonAsTheSessionOpens)
{
	validate("pc-a");
	ASSERT_TRUE(service->initiateConnection("pc-a"));

	// The remote's ready flag is processed before its answer
	service->onReadyFlagReceived("pc-a");
	ASSERT_TRUE(waitUntil([&] { return service->getConnectionState() == ConnectionStateInternal::InvitationSent && !service->isConnected(); }));
	std::this_thread::sleep_for(20ms);

	service->onReceivedAnswerToInvite("pc-a", true, "");

	EXPECT_TRUE(service->isConnected()) << "Both sides are ready: no need to wait for another flag";
}


TEST_F(ConnectionServiceTest, ReadyFlagFromAnotherPeer_IsIgnored)
{
	acceptedAsAcceptor("pc-b");

	service->onReadyFlagReceived("pc-intruder");

	std::this_thread::sleep_for(50ms);
	EXPECT_FALSE(service->isConnected());
}


TEST_F(ConnectionServiceTest, MissingReadyFlag_FailsAfterTheTimeout)
{
	ConnectionConfig cfg;
	cfg.readyFlagTimeoutMs = 100;
	service->setConfig(cfg);

	acceptedAsAcceptor();

	EXPECT_TRUE(waitUntil([&] { return received(ConnectionStatusUpdate::Type::Failed); }));
	EXPECT_EQ(service->getConnectionState(), ConnectionStateInternal::Idle);
}


TEST_F(ConnectionServiceTest, Established_DisarmsTheReadyFlagTimeout)
{
	ConnectionConfig cfg;
	cfg.readyFlagTimeoutMs = 100;
	service->setConfig(cfg);

	establish();

	std::this_thread::sleep_for(250ms);
	EXPECT_TRUE(service->isConnected());
	EXPECT_FALSE(received(ConnectionStatusUpdate::Type::Failed));
}


// ---------------------------------------------------------------------------
// Loss and teardown
// ---------------------------------------------------------------------------

TEST_F(ConnectionServiceTest, PeerLost_ClosesAnEstablishedConnection)
{
	establish();

	service->onPeerLost("pc-b", "the peer stopped acknowledging messages");

	EXPECT_TRUE(waitUntil([&] { return !service->isConnected(); }));
	EXPECT_TRUE(waitUntil([&] { return received(ConnectionStatusUpdate::Type::Closed); })) << "A lost peer must be reported as Closed";
	EXPECT_EQ(service->getConnectionState(), ConnectionStateInternal::Idle);
}


TEST_F(ConnectionServiceTest, PeerLost_WhileConnecting_Fails)
{
	acceptedAsInitiator();

	service->onPeerLost("pc-a", "no traffic from the peer anymore");

	EXPECT_TRUE(waitUntil([&] { return received(ConnectionStatusUpdate::Type::Failed); }));
	EXPECT_EQ(service->getConnectionState(), ConnectionStateInternal::Idle);
}


TEST_F(ConnectionServiceTest, PeerLost_ForAnotherPeer_IsIgnored)
{
	establish("pc-b");

	service->onPeerLost("pc-other", "whatever");

	std::this_thread::sleep_for(50ms);
	EXPECT_TRUE(service->isConnected());
}


TEST_F(ConnectionServiceTest, PeerLost_WithoutConnection_IsIgnored)
{
	service->onPeerLost("pc-b", "stale notification");

	std::this_thread::sleep_for(50ms);
	EXPECT_FALSE(received(ConnectionStatusUpdate::Type::Closed));
}


TEST_F(ConnectionServiceTest, DisconnectReceived_ClosesTheConnection)
{
	establish();

	service->onDisconnectReceived("pc-b");

	EXPECT_TRUE(waitUntil([&] { return received(ConnectionStatusUpdate::Type::Closed); }));
	EXPECT_FALSE(service->isConnected());
}


TEST_F(ConnectionServiceTest, CloseConnection_FailsWhenNoConnectionExists)
{
	EXPECT_FALSE(service->closeConnection("pc-b")) << "closeConnection() must fail when there is no active or in-progress connection";
}


TEST_F(ConnectionServiceTest, CloseConnection_ClearsInProgressConnection)
{
	validate("pc-a");
	ASSERT_TRUE(service->initiateConnection("pc-a"));

	EXPECT_TRUE(service->closeConnection("pc-a"));
	EXPECT_EQ(service->getConnectionState(), ConnectionStateInternal::Idle) << "Closing an in-progress connection must reset state back to Idle";
	EXPECT_FALSE(service->isConnecting());
}


TEST_F(ConnectionServiceTest, CloseConnection_WhenConnected_ReportsClosed)
{
	establish();

	EXPECT_TRUE(service->closeConnection("pc-b"));

	EXPECT_FALSE(service->isConnected());
	EXPECT_TRUE(received(ConnectionStatusUpdate::Type::Closing)) << "An established connection must go through the closing path (sends Disconnect to the peer)";
	EXPECT_TRUE(received(ConnectionStatusUpdate::Type::Closed));
}


// ---------------------------------------------------------------------------
// State and configuration
// ---------------------------------------------------------------------------

TEST_F(ConnectionServiceTest, InitialState_IsIdleAndNotConnected)
{
	EXPECT_EQ(service->getConnectionState(), ConnectionStateInternal::Idle) << "A freshly constructed ConnectionService must start in the Idle state";
	EXPECT_FALSE(service->isConnected());
	EXPECT_FALSE(service->isConnecting());
	EXPECT_FALSE(service->hasIncomingInvitation());
	EXPECT_FALSE(service->getCurrentRemote().has_value()) << "getCurrentRemote() must return nullopt when no connection has been initiated";
}


TEST_F(ConnectionServiceTest, SetConfig_DoesNotCrash)
{
	ConnectionConfig cfg;
	cfg.invitationTimeoutMs = 1000;
	EXPECT_NO_THROW(service->setConfig(cfg)) << "Applying a custom config must not throw";
}


TEST_F(ConnectionServiceTest, StatusCallback_FiresOnInitiate)
{
	validate("pc-a");
	service->initiateConnection("pc-a");

	EXPECT_TRUE(received(ConnectionStatusUpdate::Type::Initiated)) << "The Initiated status update must be delivered when initiateConnection() begins";
	EXPECT_TRUE(received(ConnectionStatusUpdate::Type::InvitationSent)) << "The InvitationSent status update must be delivered after successfully sending the invitation";
}

} // namespace ConnectionTests
