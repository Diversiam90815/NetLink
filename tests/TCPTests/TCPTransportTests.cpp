#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include "TCP/TCPServer.h"
#include "TCP/TCPClient.h"
#include "TCP/TCPSession.h"

using namespace netlink;
using namespace std::chrono_literals;


namespace TCPTests
{

template <typename Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout = 3s)
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


static InternalMessage makeMessage(uint32_t type, size_t size)
{
	InternalMessage message;
	message.type = type;
	message.data.resize(size);
	std::iota(message.data.begin(), message.data.end(), uint8_t{0});
	return message;
}


// Server + client connected over loopback
class TCPTransportTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		server.setSessionHandler(
			[this](ISession::pointer session)
			{
				std::lock_guard<std::mutex> lock(mutex);
				serverSession = std::move(session);
			});

		ASSERT_TRUE(server.start("127.0.0.1"));
	}

	void connect()
	{
		client.setConnectHandler(
			[this](ISession::pointer session)
			{
				std::lock_guard<std::mutex> lock(mutex);
				clientSession = std::move(session);
			});

		client.connect("127.0.0.1", static_cast<unsigned short>(server.getBoundPort()));

		ASSERT_TRUE(waitUntil([this] { return bothConnected(); })) << "Client and server must both produce a session";
	}

	bool bothConnected()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return serverSession && clientSession;
	}

	// Collects what a session delivers. Owned by the fixture so callbacks never outlive it.
	struct Inbox
	{
		std::mutex					 mutex;
		std::vector<InternalMessage> received;
		std::atomic<int>			 disconnects{0};

		size_t						 count()
		{
			std::lock_guard<std::mutex> lock(mutex);
			return received.size();
		}

		std::vector<InternalMessage> messages()
		{
			std::lock_guard<std::mutex> lock(mutex);
			return received;
		}
	};

	void startReading(const ISession::pointer &session)
	{
		session->startReadAsync(
			[this](InternalMessage message)
			{
				std::lock_guard<std::mutex> lock(inbox.mutex);
				inbox.received.push_back(std::move(message));
			},
			[this](const std::string &) { ++inbox.disconnects; });
	}

	// Declaration order matters: sessions, server & client (and their threads) are destroyed before the state their callbacks touch
	Inbox			  inbox;
	std::mutex		  mutex;
	ISession::pointer serverSession;
	ISession::pointer clientSession;

	TCPServer		  server;
	TCPClient		  client;
};


// ---------------------------------------------------------------------------
// TCPServer / TCPClient
// ---------------------------------------------------------------------------

TEST_F(TCPTransportTest, Server_BoundPortIsNonZeroAfterStart)
{
	EXPECT_NE(server.getBoundPort(), 0) << "start() must bind an OS assigned port";
}


TEST(TCPServer, StartFails_ForAddressNotOnThisMachine)
{
	TCPServer server;
	EXPECT_FALSE(server.start("203.0.113.1")) << "Listening on a foreign address must fail instead of silently accepting nothing";
	EXPECT_EQ(server.getBoundPort(), 0);
}


TEST(TCPServer, TwoInstances_GetDifferentPorts)
{
	TCPServer serverA;
	TCPServer serverB;
	ASSERT_TRUE(serverA.start("127.0.0.1"));
	ASSERT_TRUE(serverB.start("127.0.0.1"));

	EXPECT_NE(serverA.getBoundPort(), serverB.getBoundPort());
}


TEST_F(TCPTransportTest, Server_AcceptsIncomingClientConnection)
{
	connect();

	std::lock_guard<std::mutex> lock(mutex);
	EXPECT_TRUE(serverSession->isConnected());
	EXPECT_TRUE(clientSession->isConnected());
	EXPECT_EQ(serverSession->getRemoteAddress(), "127.0.0.1");
	EXPECT_EQ(serverSession->getRemotePort(), clientSession->getBoundPort()) << "Both sessions must describe the same connection";
}


TEST_F(TCPTransportTest, Server_StopKeepsEstablishedSessionsAlive)
{
	connect();
	server.stop();

	EXPECT_EQ(server.getBoundPort(), 0);

	std::lock_guard<std::mutex> lock(mutex);
	EXPECT_TRUE(serverSession->isConnected()) << "Stopping the server only stops accepting, established sessions stay usable";
}


TEST(TCPClient, ConnectTimeoutHandler_FiresOnRefusedConnection)
{
	int closedPort = 0;
	{
		TCPServer server;
		ASSERT_TRUE(server.start("127.0.0.1"));
		closedPort = server.getBoundPort();
	}

	TCPClient		  client;
	std::atomic<bool> failed{false};
	std::atomic<bool> connected{false};
	client.setConnectTimeoutHandler([&] { failed.store(true); });
	client.setConnectHandler([&](ISession::pointer) { connected.store(true); });

	client.connect("127.0.0.1", static_cast<unsigned short>(closedPort));

	EXPECT_TRUE(waitUntil([&] { return failed.load(); }, 8s)) << "A refused connection must invoke the timeout/failure handler";
	EXPECT_FALSE(connected.load());
}


TEST(TCPClient, Destructor_CancelsPendingConnectPromptly)
{
	auto	   destroyed	  = std::make_shared<std::atomic<bool>>(false);
	auto	   lateCallback	  = std::make_shared<std::atomic<bool>>(false);
	const auto started		  = std::chrono::steady_clock::now();

	{
		TCPClient client;
		client.setConnectHandler([=](ISession::pointer) { lateCallback->store(destroyed->load()); });
		client.setConnectTimeoutHandler([=] { lateCallback->store(destroyed->load()); });
		client.connect("10.255.255.1", 9); // blackholed: normally stays pending until the timeout

		std::this_thread::sleep_for(100ms);
	}
	destroyed->store(true);

	EXPECT_LT(std::chrono::steady_clock::now() - started, 2s) << "Destroying a client must not wait for the connect timeout";

	std::this_thread::sleep_for(100ms);
	EXPECT_FALSE(lateCallback->load()) << "No handler may fire after the client was destroyed";
}


// ---------------------------------------------------------------------------
// TCPSession
// ---------------------------------------------------------------------------

TEST_F(TCPTransportTest, Session_MessageRoundTrip_ClientToServer)
{
	connect();
	startReading(serverSession);

	const auto sent = makeMessage(42, 64);
	ASSERT_TRUE(clientSession->sendMessage(sent, DeliveryMode::ReliableOrdered));

	ASSERT_TRUE(waitUntil([this] { return inbox.count() == 1; }));
	auto received = inbox.messages();
	EXPECT_EQ(received[0].type, 42u);
	EXPECT_EQ(received[0].data, sent.data);
}


TEST_F(TCPTransportTest, Session_LargeMessageSplitAcrossReads)
{
	connect();
	startReading(serverSession);

	const auto sent = makeMessage(7, 1024 * 1024);
	ASSERT_TRUE(clientSession->sendMessage(sent, DeliveryMode::ReliableOrdered));

	ASSERT_TRUE(waitUntil([this] { return inbox.count() == 1; }, 10s));
	EXPECT_EQ(inbox.messages()[0].data, sent.data) << "A 1 MB message arrives in many TCP segments and must be reassembled intact";
}


TEST_F(TCPTransportTest, Session_ManyMessagesKeepOrder)
{
	connect();
	startReading(serverSession);

	const uint32_t count = 1000;
	for (uint32_t i = 0; i < count; ++i)
		ASSERT_TRUE(clientSession->sendMessage(makeMessage(i, 16), DeliveryMode::ReliableOrdered));

	ASSERT_TRUE(waitUntil([this] { return inbox.count() == count; }, 10s));

	const auto received = inbox.messages();
	for (uint32_t i = 0; i < count; ++i)
		ASSERT_EQ(received[i].type, i);
}


TEST_F(TCPTransportTest, Session_StopReadAsync_StopsDeliveringMessages)
{
	connect();
	startReading(serverSession);

	ASSERT_TRUE(clientSession->sendMessage(makeMessage(1, 4), DeliveryMode::ReliableOrdered));
	ASSERT_TRUE(waitUntil([this] { return inbox.count() == 1; }));

	serverSession->stopReadAsync();

	ASSERT_TRUE(clientSession->sendMessage(makeMessage(2, 4), DeliveryMode::ReliableOrdered));
	std::this_thread::sleep_for(300ms);

	EXPECT_EQ(inbox.count(), 1u) << "No message may be delivered after stopReadAsync() returned";
	EXPECT_TRUE(serverSession->isConnected()) << "Stopping to read does not close the connection";
}


TEST_F(TCPTransportTest, Session_IsConnected_FalseAfterClose)
{
	connect();

	clientSession->close();

	EXPECT_FALSE(clientSession->isConnected());
	EXPECT_FALSE(clientSession->sendMessage(makeMessage(1, 1), DeliveryMode::ReliableOrdered)) << "Sending on a closed session must fail";
}


TEST_F(TCPTransportTest, Session_DisconnectCallbackFiresWhenPeerCloses)
{
	connect();
	startReading(serverSession);

	clientSession->close();

	EXPECT_TRUE(waitUntil([this] { return inbox.disconnects.load() == 1; })) << "A remote close must be reported through the disconnect callback";
	EXPECT_FALSE(serverSession->isConnected()) << "isConnected() must reflect the lost connection";
}


TEST_F(TCPTransportTest, Session_LocalCloseDoesNotFireDisconnectCallback)
{
	connect();
	startReading(serverSession);

	serverSession->close();
	std::this_thread::sleep_for(200ms);

	EXPECT_EQ(inbox.disconnects.load(), 0) << "Closing locally is intentional and must not be reported as a lost connection";
}


TEST_F(TCPTransportTest, Session_ReleasedInsideItsOwnCallback_IsSafe)
{
	connect();

	{
		std::lock_guard<std::mutex> lock(mutex);
		serverSession->startReadAsync(
			[this](InternalMessage)
			{
				// Drops the last reference to the session on its own read thread
				std::lock_guard<std::mutex> callbackLock(mutex);
				serverSession.reset();
				++inbox.disconnects; // used as "released" marker
			},
			{});
	}

	ASSERT_TRUE(clientSession->sendMessage(makeMessage(1, 4), DeliveryMode::ReliableOrdered));
	EXPECT_TRUE(waitUntil([this] { return inbox.disconnects.load() == 1; }));

	std::this_thread::sleep_for(100ms); // let the detached read thread wind down
}


TEST(TCPServer, StopFromInsideSessionHandler_IsSafe)
{
	TCPServer		  server;
	std::atomic<bool> stopped{false};
	ISession::pointer accepted;

	server.setSessionHandler(
		[&](ISession::pointer session)
		{
			accepted = std::move(session);
			server.stop();
			stopped.store(true);
		});
	ASSERT_TRUE(server.start("127.0.0.1"));

	TCPClient client;
	client.setConnectHandler([](ISession::pointer) {});
	client.connect("127.0.0.1", static_cast<unsigned short>(server.getBoundPort()));

	EXPECT_TRUE(waitUntil([&] { return stopped.load(); }));
}

} // namespace TCPTests
