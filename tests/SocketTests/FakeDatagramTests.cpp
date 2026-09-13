#include <gtest/gtest.h>
#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "FakeDatagramNetwork.h"
#include "LossyDatagramSocket.h"

using namespace FakeNet;
using namespace std::chrono_literals;


namespace SocketTests
{

static std::vector<uint8_t> encode(uint32_t value)
{
	return {static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
}

static uint32_t decode(const std::vector<uint8_t> &bytes)
{
	return (uint32_t{bytes[0]} << 24) | (uint32_t{bytes[1]} << 16) | (uint32_t{bytes[2]} << 8) | uint32_t{bytes[3]};
}

static std::vector<uint32_t> drain(IDatagramSocket &socket)
{
	std::vector<uint32_t> values;
	std::vector<uint8_t>  buffer(16);

	while (auto datagram = socket.receiveFrom(buffer, 20ms))
		values.push_back(decode({buffer.begin(), buffer.begin() + datagram->size}));

	return values;
}


TEST(FakeDatagramNetwork, UnicastReachesOnlyTargetHost)
{
	auto network = FakeDatagramNetwork::create();
	auto a		 = network->factory("10.0.0.1")({"0.0.0.0", 4000}, {});
	auto b		 = network->factory("10.0.0.2")({"0.0.0.0", 4000}, {});
	ASSERT_TRUE(a && b);

	static_cast<void>((*a)->sendTo({"10.0.0.2", 4000}, encode(7)));

	EXPECT_EQ(drain(**b), std::vector<uint32_t>{7});
	EXPECT_TRUE(drain(**a).empty());
}


TEST(FakeDatagramNetwork, BroadcastReachesAllSocketsOnPort)
{
	auto network = FakeDatagramNetwork::create();

	BindOptions shared;
	shared.reuseAddress = true;

	auto a				= network->factory("10.0.0.1")({"0.0.0.0", 5555}, shared);
	auto b				= network->factory("10.0.0.2")({"0.0.0.0", 5555}, shared);
	ASSERT_TRUE(a && b);

	static_cast<void>((*a)->sendTo({BroadcastAddress, 5555}, encode(1)));

	EXPECT_EQ(drain(**a).size(), 1u) << "Like a real network, the sender receives its own broadcast";
	EXPECT_EQ(drain(**b).size(), 1u);
}


TEST(FakeDatagramNetwork, BindConflictWithoutReuse)
{
	auto network = FakeDatagramNetwork::create();
	auto first	 = network->factory("10.0.0.1")({"0.0.0.0", 4000}, {});
	auto second	 = network->factory("10.0.0.1")({"0.0.0.0", 4000}, {});

	ASSERT_TRUE(first.has_value());
	ASSERT_FALSE(second.has_value());
	EXPECT_EQ(second.error(), SocketError::AddressInUse);
}


TEST(LossyDatagramSocket, DropsApproximatelyTheConfiguredShare)
{
	auto		network = FakeDatagramNetwork::create();

	LossProfile profile;
	profile.dropRate = 0.25;

	auto sender		 = LossyDatagramSocket::wrap(network->factory("10.0.0.1"), profile)({"0.0.0.0", 0}, {});
	auto receiver	 = network->factory("10.0.0.2")({"0.0.0.0", 4000}, {});
	ASSERT_TRUE(sender && receiver);

	for (uint32_t i = 0; i < 1000; ++i)
		static_cast<void>((*sender)->sendTo({"10.0.0.2", 4000}, encode(i)));

	const auto received = drain(**receiver).size();
	EXPECT_GT(received, 650u);
	EXPECT_LT(received, 850u);
}


TEST(LossyDatagramSocket, DuplicatesEveryDatagram)
{
	auto		network = FakeDatagramNetwork::create();

	LossProfile profile;
	profile.duplicateRate = 1.0;

	auto sender			  = LossyDatagramSocket::wrap(network->factory("10.0.0.1"), profile)({"0.0.0.0", 0}, {});
	auto receiver		  = network->factory("10.0.0.2")({"0.0.0.0", 4000}, {});
	ASSERT_TRUE(sender && receiver);

	for (uint32_t i = 0; i < 10; ++i)
		static_cast<void>((*sender)->sendTo({"10.0.0.2", 4000}, encode(i)));

	EXPECT_EQ(drain(**receiver).size(), 20u);
}


TEST(LossyDatagramSocket, ReordersWithoutLosingDatagrams)
{
	auto		network = FakeDatagramNetwork::create();

	LossProfile profile;
	profile.reorderRate = 0.5;

	auto sender			= LossyDatagramSocket::wrap(network->factory("10.0.0.1"), profile)({"0.0.0.0", 0}, {});
	auto receiver		= network->factory("10.0.0.2")({"0.0.0.0", 4000}, {});
	ASSERT_TRUE(sender && receiver);

	for (uint32_t i = 0; i < 200; ++i)
		static_cast<void>((*sender)->sendTo({"10.0.0.2", 4000}, encode(i)));

	const auto values = drain(**receiver);

	EXPECT_FALSE(std::is_sorted(values.begin(), values.end())) << "Some datagrams must arrive out of order";
	EXPECT_GE(values.size(), 199u) << "Reordering must not lose datagrams (at most the last one is still held back)";
	EXPECT_EQ(std::set<uint32_t>(values.begin(), values.end()).size(), values.size()) << "Reordering must not duplicate datagrams";
}

} // namespace SocketTests
