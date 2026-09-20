#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <numeric>
#include <thread>
#include <vector>

#include "TestIp.h"
#include "Socket/TcpListener.h"
#include "Socket/TcpStream.h"

using namespace netlink::net;
using namespace std::chrono_literals;


namespace SocketTests
{

// Blackholed address: SYNs are never answered, so a connect stays pending
static const SocketAddress UnresponsiveAddress{ipv4("10.255.255.1"), 9};


struct ConnectedPair
{
	TcpListener listener;
	TcpStream	client;
	TcpStream	server;
};


static ConnectedPair connectPair()
{
	auto listener = TcpListener::listen({ipv4("127.0.0.1"), 0});
	EXPECT_TRUE(listener.has_value());

	auto pendingClient = std::async(std::launch::async, [address = listener->localAddress()] { return TcpStream::connect(address, 2s); });
	auto server		   = listener->accept(2s);
	auto client		   = pendingClient.get();

	EXPECT_TRUE(server.has_value());
	EXPECT_TRUE(client.has_value());

	return {std::move(*listener), std::move(*client), std::move(*server)};
}


TEST(TcpSocket, ListenOnPortZero_AssignsPort)
{
	auto listener = TcpListener::listen({ipv4("127.0.0.1"), 0});

	ASSERT_TRUE(listener.has_value()) << toString(listener.error());
	EXPECT_NE(listener->localAddress().port, 0);
}


TEST(TcpSocket, Accept_TimesOutWithoutClient)
{
	auto listener = TcpListener::listen({ipv4("127.0.0.1"), 0});
	ASSERT_TRUE(listener);

	auto stream = listener->accept(50ms);

	ASSERT_FALSE(stream.has_value());
	EXPECT_EQ(stream.error(), SocketError::Timeout);
}


TEST(TcpSocket, ConnectAndAccept_ExchangeBytesBothWays)
{
	auto pair = connectPair();

	EXPECT_EQ(pair.server.remoteAddress().port, pair.client.localAddress().port) << "Both ends must agree on the connection's endpoints";

	const std::vector<uint8_t> ping{1, 2, 3, 4};
	ASSERT_TRUE(pair.client.sendAll(ping, 1s));

	std::vector<uint8_t> buffer(16);
	auto				 received = pair.server.receiveSome(buffer, 2s);
	ASSERT_TRUE(received.has_value());
	EXPECT_EQ(std::vector<uint8_t>(buffer.begin(), buffer.begin() + *received), ping);

	const std::vector<uint8_t> pong{9, 8, 7};
	ASSERT_TRUE(pair.server.sendAll(pong, 1s));

	received = pair.client.receiveSome(buffer, 2s);
	ASSERT_TRUE(received.has_value());
	EXPECT_EQ(std::vector<uint8_t>(buffer.begin(), buffer.begin() + *received), pong);
}


TEST(TcpSocket, SendAll_LargeBufferArrivesCompletely)
{
	auto				 pair = connectPair();

	std::vector<uint8_t> payload(4 * 1024 * 1024);
	std::iota(payload.begin(), payload.end(), uint8_t{0});

	// Reader drains concurrently, otherwise the socket buffers fill up and sendAll times out by design
	auto reader = std::async(std::launch::async,
							 [&]
							 {
								 std::vector<uint8_t> received;
								 std::vector<uint8_t> buffer(64 * 1024);
								 while (received.size() < payload.size())
								 {
									 auto chunk = pair.server.receiveSome(buffer, 2s);
									 if (!chunk)
										 break;
									 received.insert(received.end(), buffer.begin(), buffer.begin() + *chunk);
								 }
								 return received;
							 });

	ASSERT_TRUE(pair.client.sendAll(payload, 5s));
	EXPECT_EQ(reader.get(), payload);
}


TEST(TcpSocket, ReceiveSome_TimesOutWhenNothingArrives)
{
	auto				 pair = connectPair();
	std::vector<uint8_t> buffer(16);

	auto				 received = pair.server.receiveSome(buffer, 50ms);

	ASSERT_FALSE(received.has_value());
	EXPECT_EQ(received.error(), SocketError::Timeout);
}


TEST(TcpSocket, PeerClose_ReportsClosed)
{
	auto pair = connectPair();

	{
		TcpStream closing = std::move(pair.client); // destroyed at scope end
	}

	std::vector<uint8_t> buffer(16);
	auto				 received = pair.server.receiveSome(buffer, 2s);

	ASSERT_FALSE(received.has_value());
	EXPECT_TRUE(received.error() == SocketError::Closed || received.error() == SocketError::ConnectionReset) << toString(received.error());
}


TEST(TcpSocket, Shutdown_WakesBlockedReceive)
{
	auto pair	 = connectPair();

	auto blocked = std::async(std::launch::async,
							  [&]
							  {
								  std::vector<uint8_t> buffer(16);
								  const auto		   started = std::chrono::steady_clock::now();
								  auto				   result  = pair.server.receiveSome(buffer, 10s);
								  return std::make_pair(result.has_value(), std::chrono::steady_clock::now() - started);
							  });

	std::this_thread::sleep_for(100ms);
	pair.server.shutdown();

	auto [succeeded, elapsed] = blocked.get();
	EXPECT_FALSE(succeeded);
	EXPECT_LT(elapsed, 5s) << "shutdown() from another thread must wake a pending receive";
}


TEST(TcpSocket, ConnectToClosedPort_FailsRefused)
{
	SocketAddress closedAddress;
	{
		auto listener = TcpListener::listen({ipv4("127.0.0.1"), 0});
		ASSERT_TRUE(listener);
		closedAddress = listener->localAddress();
	}

	auto stream = TcpStream::connect(closedAddress, 5s);

	ASSERT_FALSE(stream.has_value());
	EXPECT_EQ(stream.error(), SocketError::ConnectionRefused);
}


TEST(TcpSocket, Connect_CanBeCancelled)
{
	std::atomic<bool> cancel{false};

	auto			  pending = std::async(std::launch::async,
										   [&]
										   {
								  const auto started = std::chrono::steady_clock::now();
								  auto		 result	 = TcpStream::connect(UnresponsiveAddress, 10s, [&] { return cancel.load(); });
								  return std::make_pair(std::move(result), std::chrono::steady_clock::now() - started);
										   });

	std::this_thread::sleep_for(100ms);
	cancel.store(true);

	auto [result, elapsed] = pending.get();

	ASSERT_FALSE(result.has_value());
	EXPECT_NE(result.error(), SocketError::Timeout);
	EXPECT_LT(elapsed, 2s) << "A cancelled connect must return promptly";
}


TEST(TcpSocket, MalformedAddressIsRejectedBeforeItCanReachConnect)
{
	// SocketAddress holds an IPv4Address, so connect() can no longer be handed a
	// malformed address: it is rejected at parse time instead.
	EXPECT_FALSE(netlink::net::IPv4Address::parse("999.1.1.1").has_value());
}


TEST(TcpSocket, Connect_UnspecifiedAddress_FailsWithInvalidArgument)
{
	auto stream = TcpStream::connect({netlink::net::IPv4Address{}, 80}, 1s);

	ASSERT_FALSE(stream.has_value());
	EXPECT_EQ(stream.error(), SocketError::InvalidArgument);
}

} // namespace SocketTests
