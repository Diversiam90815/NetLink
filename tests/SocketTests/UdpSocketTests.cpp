#include <gtest/gtest.h>
#include <chrono>
#include <string>
#include <vector>

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
	auto socket = UdpSocket::bind({"127.0.0.1", 0});

	ASSERT_TRUE(socket.has_value()) << toString(socket.error());
	EXPECT_NE(socket->localAddress().port, 0) << "Binding port 0 must report the OS assigned port";
	EXPECT_EQ(socket->localAddress().ip, "127.0.0.1");
}


TEST(UdpSocket, SendAndReceiveOverLoopback)
{
	auto receiver = UdpSocket::bind({"127.0.0.1", 0});
	auto sender	  = UdpSocket::bind({"127.0.0.1", 0});
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


TEST(UdpSocket, Receive_TimesOutWhenNothingArrives)
{
	auto socket = UdpSocket::bind({"127.0.0.1", 0});
	ASSERT_TRUE(socket);

	std::vector<uint8_t> buffer(64);
	const auto			 started  = std::chrono::steady_clock::now();
	auto				 datagram = socket->receiveFrom(buffer, 50ms);

	ASSERT_FALSE(datagram.has_value());
	EXPECT_EQ(datagram.error(), SocketError::Timeout);
	EXPECT_LT(std::chrono::steady_clock::now() - started, 1s) << "The timeout must be honored";
}


TEST(UdpSocket, SecondBindWithoutReuse_FailsWithAddressInUse)
{
	auto first = UdpSocket::bind({"127.0.0.1", 0});
	ASSERT_TRUE(first);

	auto second = UdpSocket::bind(first->localAddress());

	ASSERT_FALSE(second.has_value());
	EXPECT_EQ(second.error(), SocketError::AddressInUse);
}


TEST(UdpSocket, ReuseAddress_AllowsSharedPort)
{
	BindOptions options;
	options.reuseAddress = true;

	auto first			 = UdpSocket::bind({"0.0.0.0", 0}, options);
	ASSERT_TRUE(first);

	auto second = UdpSocket::bind({"0.0.0.0", first->localAddress().port}, options);
	EXPECT_TRUE(second.has_value()) << "Two sockets with reuseAddress must be able to share a port (needed for LAN discovery)";
}


TEST(UdpSocket, InvalidAddress_FailsWithInvalidArgument)
{
	auto socket = UdpSocket::bind({"not-an-ip", 0});

	ASSERT_FALSE(socket.has_value());
	EXPECT_EQ(socket.error(), SocketError::InvalidArgument);
}


TEST(UdpSocket, Factory_CreatesBoundSocket)
{
	auto factory = UdpSocket::factory();
	auto socket	 = factory({"127.0.0.1", 0}, {});

	ASSERT_TRUE(socket.has_value());
	EXPECT_NE((*socket)->localAddress().port, 0);
}

} // namespace SocketTests
