#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "TestIp.h"
#include "Socket/UdpSocket.h"

using namespace netlink::net;
using namespace std::chrono_literals;


namespace SocketTests
{

static std::span<const uint8_t> asBytes(const std::string &text)
{
	return {reinterpret_cast<const uint8_t *>(text.data()), text.size()};
}


TEST(UdpSocket, BindToPortZero_AssignsPort)
{
	auto socket = UdpSocket::bind({ipv4("127.0.0.1"), 0});

	ASSERT_TRUE(socket.has_value()) << toString(socket.error());
	EXPECT_NE(socket->localAddress().port, 0) << "Binding port 0 must report the OS assigned port";
	EXPECT_EQ(socket->localAddress().ip, ipv4("127.0.0.1"));
}


TEST(UdpSocket, SendAndReceiveOverLoopback)
{
	auto receiver = UdpSocket::bind({ipv4("127.0.0.1"), 0});
	auto sender	  = UdpSocket::bind({ipv4("127.0.0.1"), 0});
	ASSERT_TRUE(receiver && sender);

	auto sent = sender->sendTo(receiver->localAddress(), asBytes("hello"));
	ASSERT_TRUE(sent.has_value()) << toString(sent.error());
	EXPECT_EQ(*sent, 5u);

	std::vector<uint8_t> buffer(1024);
	auto				 datagram = receiver->receiveFrom(buffer, 2s);

	ASSERT_TRUE(datagram.has_value()) << toString(datagram.error());
	EXPECT_EQ(std::string(buffer.begin(), buffer.begin() + datagram->size), "hello");
	EXPECT_EQ(datagram->from, sender->localAddress()) << "The sender address must be reported";
}


TEST(UdpSocket, SendParts_ArrivesAsOneDatagram)
{
	auto receiver = UdpSocket::bind({ipv4("127.0.0.1"), 0});
	auto sender	  = UdpSocket::bind({ipv4("127.0.0.1"), 0});
	ASSERT_TRUE(receiver && sender);

	auto sent = sender->sendParts(receiver->localAddress(), asBytes("head:"), asBytes("body"));
	ASSERT_TRUE(sent.has_value()) << toString(sent.error());
	EXPECT_EQ(*sent, 9u);

	auto headOnly = sender->sendParts(receiver->localAddress(), asBytes("alone"), {});
	ASSERT_TRUE(headOnly.has_value()) << toString(headOnly.error());
	EXPECT_EQ(*headOnly, 5u);

	std::vector<uint8_t> buffer(1024);

	auto				 first = receiver->receiveFrom(buffer, 2s);
	ASSERT_TRUE(first.has_value()) << toString(first.error());
	EXPECT_EQ(std::string(buffer.begin(), buffer.begin() + first->size), "head:body") << "Both parts must leave as a single datagram";

	auto second = receiver->receiveFrom(buffer, 2s);
	ASSERT_TRUE(second.has_value()) << toString(second.error());
	EXPECT_EQ(std::string(buffer.begin(), buffer.begin() + second->size), "alone") << "An empty body is allowed";
}


TEST(UdpSocket, Receive_TimesOutWhenNothingArrives)
{
	auto socket = UdpSocket::bind({ipv4("127.0.0.1"), 0});
	ASSERT_TRUE(socket);

	std::vector<uint8_t> buffer(64);
	const auto			 started  = std::chrono::steady_clock::now();
	auto				 datagram = socket->receiveFrom(buffer, 50ms);

	ASSERT_FALSE(datagram.has_value());
	EXPECT_EQ(datagram.error(), SocketError::Timeout);
	EXPECT_LT(std::chrono::steady_clock::now() - started, 1s) << "The timeout must be honored";
}


TEST(UdpSocket, ReceiveWithZeroTimeout_DoesNotBlock)
{
	auto receiver = UdpSocket::bind({ipv4("127.0.0.1"), 0});
	auto sender	  = UdpSocket::bind({ipv4("127.0.0.1"), 0});
	ASSERT_TRUE(receiver && sender);

	std::vector<uint8_t> buffer(64);

	auto				 nothing = receiver->receiveFrom(buffer, 0ms);
	ASSERT_FALSE(nothing.has_value());
	EXPECT_EQ(nothing.error(), SocketError::Timeout);

	ASSERT_TRUE(sender->sendTo(receiver->localAddress(), asBytes("ping")));
	ASSERT_TRUE(receiver->waitReadable(2s)) << "Loopback delivery must make the socket readable";

	auto datagram = receiver->receiveFrom(buffer, 0ms);
	ASSERT_TRUE(datagram.has_value()) << "A waiting datagram must be returned without waiting";
	EXPECT_EQ(datagram->size, 4u);
}


TEST(UdpSocket, WaitReadable_DoesNotConsumeTheDatagram)
{
	auto receiver = UdpSocket::bind({ipv4("127.0.0.1"), 0});
	auto sender	  = UdpSocket::bind({ipv4("127.0.0.1"), 0});
	ASSERT_TRUE(receiver && sender);

	auto idle = receiver->waitReadable(20ms);
	ASSERT_FALSE(idle.has_value());
	EXPECT_EQ(idle.error(), SocketError::Timeout);

	ASSERT_TRUE(sender->sendTo(receiver->localAddress(), asBytes("one")));
	ASSERT_TRUE(sender->sendTo(receiver->localAddress(), asBytes("two")));

	std::vector<uint8_t> buffer(64);

	for (const std::string expected : {"one", "two"})
	{
		ASSERT_TRUE(receiver->waitReadable(2s)) << "Still readable while a datagram is waiting (" << expected << ")";

		auto datagram = receiver->receiveFrom(buffer, 0ms);
		ASSERT_TRUE(datagram.has_value());
		EXPECT_EQ(std::string(buffer.begin(), buffer.begin() + datagram->size), expected);
	}
}


TEST(UdpSocket, Interrupt_EndsAWaitingReceive)
{
	auto socket = UdpSocket::bind({ipv4("127.0.0.1"), 0});
	ASSERT_TRUE(socket);

	auto waiting = std::async(std::launch::async,
							  [&]
							  {
								  std::vector<uint8_t> buffer(64);
								  return socket->receiveFrom(buffer, 10s);
							  });

	std::this_thread::sleep_for(50ms);
	socket->interrupt();

	ASSERT_EQ(waiting.wait_for(2s), std::future_status::ready) << "interrupt() must end the wait long before its timeout";

	const auto datagram = waiting.get();
	ASSERT_FALSE(datagram.has_value());
	EXPECT_EQ(datagram.error(), SocketError::Cancelled);
}


TEST(UdpSocket, InterruptBeforeTheWait_EndsTheNextWaitOnly)
{
	auto socket = UdpSocket::bind({ipv4("127.0.0.1"), 0});
	ASSERT_TRUE(socket);

	socket->interrupt();

	const auto first = socket->waitReadable(5s);
	ASSERT_FALSE(first.has_value());
	EXPECT_EQ(first.error(), SocketError::Cancelled) << "An interrupt without a wait in progress must not be lost";

	const auto second = socket->waitReadable(20ms);
	ASSERT_FALSE(second.has_value());
	EXPECT_EQ(second.error(), SocketError::Timeout) << "One interrupt ends exactly one wait";
}


TEST(UdpSocket, Shutdown_EndsAWaitingReceive)
{
	auto socket = UdpSocket::bind({ipv4("127.0.0.1"), 0});
	ASSERT_TRUE(socket);

	auto waiting = std::async(std::launch::async,
							  [&]
							  {
								  std::vector<uint8_t> buffer(64);
								  return socket->receiveFrom(buffer, 10s);
							  });

	std::this_thread::sleep_for(50ms);
	socket->shutdown();

	ASSERT_EQ(waiting.wait_for(2s), std::future_status::ready);

	const auto datagram = waiting.get();
	ASSERT_FALSE(datagram.has_value());
	EXPECT_EQ(datagram.error(), SocketError::Closed);
}


TEST(UdpSocket, ShortReceiveTimeouts_EndCloseToTheirDeadline)
{
	using Clock = std::chrono::steady_clock;

	auto socket = UdpSocket::bind({ipv4("127.0.0.1"), 0});
	ASSERT_TRUE(socket);

	std::vector<uint8_t> buffer(64);

	// The best of several attempts: a busy machine may delay single ones, a coarse timer delays all of them
	auto				 smallestDelay = Clock::duration::max();

	for (int attempt = 0; attempt < 20; ++attempt)
	{
		const auto deadline = Clock::now() + 1ms;
		ASSERT_FALSE(socket->receiveFrom(buffer, 1ms).has_value());
		smallestDelay = std::min(smallestDelay, Clock::now() - deadline);
	}

	EXPECT_GE(smallestDelay, Clock::duration::zero()) << "The wait must not end before its timeout";
	EXPECT_LT(smallestDelay, 5ms) << "A 1 ms timeout must not be rounded up to the scheduler tick (15.6 ms on Windows)";
}


TEST(UdpSocket, SecondBindWithoutReuse_FailsWithAddressInUse)
{
	auto first = UdpSocket::bind({ipv4("127.0.0.1"), 0});
	ASSERT_TRUE(first);

	auto second = UdpSocket::bind(first->localAddress());

	ASSERT_FALSE(second.has_value());
	EXPECT_EQ(second.error(), SocketError::AddressInUse);
}


TEST(UdpSocket, ReuseAddress_AllowsSharedPort)
{
	BindOptions options;
	options.reuseAddress = true;

	auto first			 = UdpSocket::bind({ipv4("0.0.0.0"), 0}, options);
	ASSERT_TRUE(first);

	auto second = UdpSocket::bind({ipv4("0.0.0.0"), first->localAddress().port}, options);
	EXPECT_TRUE(second.has_value()) << "Two sockets with reuseAddress must be able to share a port (needed for LAN discovery)";
}


TEST(UdpSocket, MalformedAddressIsRejectedBeforeItCanReachBind)
{
	// SocketAddress holds an IPv4Address, so a malformed address is rejected at parse
	// time and bind() can no longer be reached with one.
	EXPECT_FALSE(IPv4Address::parse("not-an-ip").has_value());
	EXPECT_FALSE(IPv4Address::parse("300.1.1.1").has_value());
}


TEST(UdpSocket, Factory_CreatesBoundSocket)
{
	auto factory = UdpSocket::factory();
	auto socket	 = factory({ipv4("127.0.0.1"), 0}, {});

	ASSERT_TRUE(socket.has_value());
	EXPECT_NE((*socket)->localAddress().port, 0);
}

} // namespace SocketTests
