#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "TCP/TCPServer.h"
#include "TCP/TCPClient.h"
#include "TCP/TCPSession.h"
#include "Socket/NetlinkSocket.h"

using namespace std::chrono_literals;


namespace TCPTests
{

template <typename Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout = 2s)
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


class TCPTransportTest : public ::testing::Test
{
};



// ---------------------------------------------------------------------------
// TCPServer
// ---------------------------------------------------------------------------

TEST_F(TCPTransportTest, Server_BoundPortIsNonZeroAfterStartAccept)
{
	TCPServer server;
	ASSERT_TRUE(server.bindAndListen("127.0.0.1", 0));
	server.setSessionHandler([](ISession::pointer) {});
	server.startAccept();

	EXPECT_NE(server.getBoundPort(), 0) << "TCPServer must report a non-zero OS-assigned port once startAccept() has been called";
}


TEST_F(TCPTransportTest, Server_TwoInstances_GetDifferentPorts)
{
	TCPServer serverA;
	TCPServer serverB;
	ASSERT_TRUE(serverA.bindAndListen("127.0.0.1", 0));
	ASSERT_TRUE(serverB.bindAndListen("127.0.0.1", 0));
	serverA.setSessionHandler([](ISession::pointer) {});
	serverB.setSessionHandler([](ISession::pointer) {});
	serverA.startAccept();
	serverB.startAccept();

	EXPECT_NE(serverA.getBoundPort(), serverB.getBoundPort()) << "Two independently constructed servers must be bound to different OS-assigned ports once accepting";
}


TEST_F(TCPTransportTest, Server_RespondToConnectionRequest_WithoutPendingSession_DoesNotCrash)
{
	TCPServer server;
	EXPECT_NO_THROW(server.respondToConnectionRequest(true)) << "respondToConnectionRequest() must be safe to call even when no connection is pending";
	EXPECT_NO_THROW(server.respondToConnectionRequest(false));
}


TEST_F(TCPTransportTest, Server_AcceptsIncomingClientConnection)
{
	TCPServer server;
	ASSERT_TRUE(server.bindAndListen("127.0.0.1", 0));

	std::atomic<bool> sessionHandlerCalled{false};
	ISession::pointer serverSession;
	server.setSessionHandler(
		[&](ISession::pointer session)
		{
			serverSession = session;
			sessionHandlerCalled.store(true);
		});
	server.startAccept();

	TCPClient		  client;
	std::atomic<bool> clientConnected{false};
	client.setConnectHandler([&](ISession::pointer) { clientConnected.store(true); });

	client.connect("127.0.0.1", static_cast<unsigned short>(server.getBoundPort()));

	EXPECT_TRUE(waitUntil([&] { return sessionHandlerCalled.load(); })) << "Connecting a TCPClient to a listening TCPServer must invoke the server's session handler";
	EXPECT_TRUE(waitUntil([&] { return clientConnected.load(); })) << "A successful TCP connection must invoke the client's connect handler";
	ASSERT_TRUE(serverSession != nullptr);
	EXPECT_TRUE(serverSession->isConnected()) << "The session created by the server upon accepting a connection must report as connected";
}


// ---------------------------------------------------------------------------
// TCPClient
// ---------------------------------------------------------------------------

TEST_F(TCPTransportTest, Client_ConnectTimeoutHandler_FiresOnRefusedConnection)
{
	TCPClient		  client;

	std::atomic<bool> timedOutOrRefused{false};
	client.setConnectTimeoutHandler([&] { timedOutOrRefused.store(true); });
	client.setConnectHandler([&](ISession::pointer) {});

	// Nothing is listening on this high, unlikely-to-be-used port -> connection should be refused quickly.
	client.connect("127.0.0.1", 1); // port 1 is a privileged/reserved port, virtually never listening

	EXPECT_TRUE(waitUntil([&] { return timedOutOrRefused.load(); }, 3s))
		<< "Connecting to a port with nothing listening must eventually invoke the connect-timeout handler (connection refused)";
}


TEST_F(TCPTransportTest, Client_SuccessfulConnect_ProvidesUsableSession)
{
	TCPServer server;
	ASSERT_TRUE(server.bindAndListen("127.0.0.1", 0));
	server.startAccept();
	server.setSessionHandler([](ISession::pointer) {});

	TCPClient		  client;
	ISession::pointer clientSession;
	std::atomic<bool> connected{false};
	client.setConnectHandler(
		[&](ISession::pointer session)
		{
			clientSession = session;
			connected.store(true);
		});

	client.connect("127.0.0.1", static_cast<unsigned short>(server.getBoundPort()));

	ASSERT_TRUE(waitUntil([&] { return connected.load(); }));
	ASSERT_TRUE(clientSession != nullptr);
	EXPECT_TRUE(clientSession->isConnected()) << "The session handed to the client's connect handler must be connected";
	EXPECT_NE(clientSession->getBoundPort(), 0) << "The client-side session must have a valid bound local port";
}


// ---------------------------------------------------------------------------
// TCPSession — message round trip
// ---------------------------------------------------------------------------

TEST_F(TCPTransportTest, Session_SendMessage_FailsWhenNotConnected)
{
	auto					 session = std::make_shared<TCPSession>(NetlinkSocket::createTCP()); // unconnected socket

	netlink::InternalMessage msg;
	msg.type = 42;
	msg.data = {1, 2, 3};

	EXPECT_FALSE(session->sendMessage(msg)) << "sendMessage() must fail on a session whose socket has not been connected yet";
}


TEST_F(TCPTransportTest, Session_StartReadAsync_FailsWhenNotConnected)
{
	auto			  session = std::make_shared<TCPSession>(NetlinkSocket::createTCP());
	std::atomic<bool> callbackCalled{false};

	// Must not crash; the read loop simply never delivers anything on a disconnected socket.
	EXPECT_NO_THROW(session->startReadAsync([&](netlink::InternalMessage &) { callbackCalled.store(true); }));

	std::this_thread::sleep_for(100ms);
	EXPECT_FALSE(callbackCalled.load()) << "startReadAsync() on a disconnected socket must not begin delivering messages";

	session->stopReadAsync();
}


TEST_F(TCPTransportTest, Session_MessageRoundTrip_ClientToServer)
{
	TCPServer server;
	ASSERT_TRUE(server.bindAndListen("127.0.0.1", 0));
	server.startAccept();

	ISession::pointer serverSession;
	std::atomic<bool> serverGotSession{false};
	server.setSessionHandler(
		[&](ISession::pointer session)
		{
			serverSession = session;
			serverGotSession.store(true);
		});

	TCPClient		  client;
	ISession::pointer clientSession;
	std::atomic<bool> clientConnected{false};
	client.setConnectHandler(
		[&](ISession::pointer session)
		{
			clientSession = session;
			clientConnected.store(true);
		});

	client.connect("127.0.0.1", static_cast<unsigned short>(server.getBoundPort()));

	ASSERT_TRUE(waitUntil([&] { return serverGotSession.load() && clientConnected.load(); }));

	std::atomic<bool>		 messageReceived{false};
	netlink::InternalMessage received;
	serverSession->startReadAsync(
		[&](netlink::InternalMessage &message)
		{
			received = message;
			messageReceived.store(true);
		});

	netlink::InternalMessage toSend;
	toSend.type = 7;
	toSend.data = {10, 20, 30, 40};

	ASSERT_TRUE(clientSession->sendMessage(toSend)) << "sendMessage() must succeed on a fully connected session";

	EXPECT_TRUE(waitUntil([&] { return messageReceived.load(); })) << "The server-side session must receive the message sent by the client via its async read loop";
	EXPECT_EQ(received.type, 7u) << "The received message type must match what was sent";
	EXPECT_EQ(received.data, std::vector<uint8_t>({10, 20, 30, 40})) << "The received message payload must exactly match what was sent";
}


TEST_F(TCPTransportTest, Session_StopReadAsync_StopsDeliveringMessages)
{
	TCPServer server;
	ASSERT_TRUE(server.bindAndListen("127.0.0.1", 0));
	server.startAccept();

	ISession::pointer serverSession;
	std::atomic<bool> serverGotSession{false};
	server.setSessionHandler(
		[&](ISession::pointer session)
		{
			serverSession = session;
			serverGotSession.store(true);
		});

	TCPClient		  client;
	ISession::pointer clientSession;
	std::atomic<bool> clientConnected{false};
	client.setConnectHandler(
		[&](ISession::pointer session)
		{
			clientSession = session;
			clientConnected.store(true);
		});

	client.connect("127.0.0.1", static_cast<unsigned short>(server.getBoundPort()));
	ASSERT_TRUE(waitUntil([&] { return serverGotSession.load() && clientConnected.load(); }));

	std::atomic<int> messageCount{0};
	serverSession->startReadAsync([&](netlink::InternalMessage &) { ++messageCount; });
	serverSession->stopReadAsync();

	netlink::InternalMessage msg;
	msg.type = 1;
	msg.data = {0xAB};
	clientSession->sendMessage(msg);

	std::this_thread::sleep_for(300ms);
	EXPECT_EQ(messageCount.load(), 0) << "After stopReadAsync(), no further messages must be delivered even if bytes arrive on the wire";
}


TEST_F(TCPTransportTest, Session_IsConnected_FalseAfterSocketClosed)
{
	TCPServer server;
	ASSERT_TRUE(server.bindAndListen("127.0.0.1", 0));
	server.startAccept();

	ISession::pointer serverSession;
	std::atomic<bool> serverGotSession{false};
	server.setSessionHandler(
		[&](ISession::pointer session)
		{
			serverSession = session;
			serverGotSession.store(true);
		});

	TCPClient		  client;
	ISession::pointer clientSession;
	std::atomic<bool> clientConnected{false};
	client.setConnectHandler(
		[&](ISession::pointer session)
		{
			clientSession = session;
			clientConnected.store(true);
		});

	client.connect("127.0.0.1", static_cast<unsigned short>(server.getBoundPort()));
	ASSERT_TRUE(waitUntil([&] { return serverGotSession.load() && clientConnected.load(); }));

	ASSERT_TRUE(clientSession->isConnected());

	// Close via ISession's own close() (added to the interface for exactly this purpose)
	// instead of reaching into an asio-specific socket() accessor.
	clientSession->close();

	EXPECT_FALSE(clientSession->isConnected()) << "isConnected() must return false once the underlying socket has been closed";
}


// ---------------------------------------------------------------------------
// TCPServer — respondToConnectionRequest
// ---------------------------------------------------------------------------

TEST_F(TCPTransportTest, Server_RespondToConnectionRequest_Accepted_InvokesSessionHandlerAgain)
{
	TCPServer server;
	ASSERT_TRUE(server.bindAndListen("127.0.0.1", 0));
	server.startAccept();

	std::atomic<int> sessionHandlerCallCount{0};
	server.setSessionHandler([&](ISession::pointer) { ++sessionHandlerCallCount; });

	TCPClient client;
	client.setConnectHandler([](ISession::pointer) {});
	client.connect("127.0.0.1", static_cast<unsigned short>(server.getBoundPort()));

	ASSERT_TRUE(waitUntil([&] { return sessionHandlerCallCount.load() >= 1; }));

	server.respondToConnectionRequest(true);

	EXPECT_TRUE(waitUntil([&] { return sessionHandlerCallCount.load() >= 2; }))
		<< "respondToConnectionRequest(true) must re-invoke the session handler with the (still valid) pending session";
}


TEST_F(TCPTransportTest, Server_RespondToConnectionRequest_Declined_ClosesSocket)
{
	TCPServer server;
	ASSERT_TRUE(server.bindAndListen("127.0.0.1", 0));
	server.startAccept();

	ISession::pointer pendingSession;
	std::atomic<bool> sessionHandlerCalled{false};
	server.setSessionHandler(
		[&](ISession::pointer session)
		{
			pendingSession = session;
			sessionHandlerCalled.store(true);
		});

	TCPClient client;
	client.setConnectHandler([](ISession::pointer) {});
	client.connect("127.0.0.1", static_cast<unsigned short>(server.getBoundPort()));

	ASSERT_TRUE(waitUntil([&] { return sessionHandlerCalled.load(); }));
	ASSERT_TRUE(pendingSession != nullptr);

	server.respondToConnectionRequest(false);

	EXPECT_TRUE(waitUntil([&] { return !pendingSession->isConnected(); }))
		<< "respondToConnectionRequest(false) must close the pending session's socket, making it report as disconnected";
}

} // namespace TCPTests
