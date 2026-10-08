#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "TestIp.h"
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
	auto a		 = network->factory("10.0.0.1")({ipv4("0.0.0.0"), 4000}, {});
	auto b		 = network->factory("10.0.0.2")({ipv4("0.0.0.0"), 4000}, {});
	ASSERT_TRUE(a && b);

	static_cast<void>((*a)->sendTo({ipv4("10.0.0.2"), 4000}, encode(7)));

	EXPECT_EQ(drain(**b), std::vector<uint32_t>{7});
	EXPECT_TRUE(drain(**a).empty());
}


TEST(FakeDatagramNetwork, BroadcastReachesAllSocketsOnPort)
{
	auto		network = FakeDatagramNetwork::create();

	BindOptions shared;
	shared.reuseAddress = true;

	auto a				= network->factory("10.0.0.1")({ipv4("0.0.0.0"), 5555}, shared);
	auto b				= network->factory("10.0.0.2")({ipv4("0.0.0.0"), 5555}, shared);
	ASSERT_TRUE(a && b);

	static_cast<void>((*a)->sendTo({ipv4(BroadcastAddress), 5555}, encode(1)));

	EXPECT_EQ(drain(**a).size(), 1u) << "Like a real network, the sender receives its own broadcast";
	EXPECT_EQ(drain(**b).size(), 1u);
}


TEST(FakeDatagramNetwork, UnicastToASharedPort_ReachesExactlyOneSocket)
{
	auto		network = FakeDatagramNetwork::create();

	BindOptions shared;
	shared.reuseAddress = true;

	auto first			= network->factory("10.0.0.2")({ipv4("0.0.0.0"), 5555}, shared);
	auto second			= network->factory("10.0.0.2")({ipv4("0.0.0.0"), 5555}, shared);
	auto sender			= network->factory("10.0.0.1")({ipv4("0.0.0.0"), 0}, {});
	ASSERT_TRUE(first && second && sender);

	static_cast<void>((*sender)->sendTo({ipv4("10.0.0.2"), 5555}, encode(7)));

	EXPECT_EQ(drain(**first).size() + drain(**second).size(), 1u) << "Like on a real host, sockets that share a port do not all get a unicast datagram";
}


TEST(FakeDatagramNetwork, SubnetBroadcast_StaysInItsSubnet)
{
	auto		network = FakeDatagramNetwork::create();

	BindOptions shared;
	shared.reuseAddress = true;

	auto sender			= network->factory("10.0.0.1")({ipv4("0.0.0.0"), 5555}, shared);
	auto neighbour		= network->factory("10.0.0.2")({ipv4("0.0.0.0"), 5555}, shared);
	auto elsewhere		= network->factory("10.0.1.2")({ipv4("0.0.0.0"), 5555}, shared);
	ASSERT_TRUE(sender && neighbour && elsewhere);

	static_cast<void>((*sender)->sendTo({ipv4("10.0.0.255"), 5555}, encode(1)));

	EXPECT_EQ(drain(**neighbour).size(), 1u);
	EXPECT_EQ(drain(**sender).size(), 1u);
	EXPECT_TRUE(drain(**elsewhere).empty()) << "10.0.1.0/24 is another subnet";
}


TEST(FakeDatagramNetwork, UnboundSocket_SendsFromTheInterfaceTowardsTheDestination)
{
	auto network  = FakeDatagramNetwork::create();
	auto twoHomed = network->factory(std::vector<Interface>{{.ip = ipv4("10.0.0.1")}, {.ip = ipv4("192.168.1.1")}})({ipv4("0.0.0.0"), 4000}, {});
	auto left	  = network->factory("10.0.0.2")({ipv4("0.0.0.0"), 4000}, {});
	auto right	  = network->factory("192.168.1.2")({ipv4("0.0.0.0"), 4000}, {});
	ASSERT_TRUE(twoHomed && left && right);

	static_cast<void>((*twoHomed)->sendTo({ipv4("10.0.0.2"), 4000}, encode(1)));
	static_cast<void>((*twoHomed)->sendTo({ipv4("192.168.1.2"), 4000}, encode(2)));

	std::vector<uint8_t> buffer(16);

	const auto			 atLeft = (*left)->receiveFrom(buffer, 20ms);
	ASSERT_TRUE(atLeft.has_value());
	EXPECT_EQ(atLeft->from.ip, ipv4("10.0.0.1"));

	const auto atRight = (*right)->receiveFrom(buffer, 20ms);
	ASSERT_TRUE(atRight.has_value());
	EXPECT_EQ(atRight->from.ip, ipv4("192.168.1.1")) << "The receiver sees the address of the interface it can answer to";
}


TEST(FakeDatagramNetwork, VirtualTime_DatagramsArriveWhenTheirTimeHasCome)
{
	auto	   network = FakeDatagramNetwork::create();
	const auto start   = Clock::now();
	network->useVirtualTime(start);

	LinkProfile profile;
	profile.latency = 5ms;
	network->setProfile(profile);

	auto sender	  = network->factory("10.0.0.1")({ipv4("0.0.0.0"), 0}, {});
	auto receiver = network->factory("10.0.0.2")({ipv4("0.0.0.0"), 4000}, {});
	ASSERT_TRUE(sender && receiver);

	static_cast<void>((*sender)->sendTo({ipv4("10.0.0.2"), 4000}, encode(1)));

	std::vector<uint8_t> buffer(16);
	EXPECT_FALSE((*receiver)->receiveFrom(buffer, 10s).has_value()) << "Still on its way, and under a virtual clock nothing waits";
	EXPECT_EQ(network->nextArrival(), start + 5ms);

	network->advanceTo(start + 5ms);
	EXPECT_TRUE((*receiver)->receiveFrom(buffer, 0ms).has_value());
}


TEST(FakeDatagramNetwork, Bandwidth_QueuesDatagramsAndAFullQueueDropsOrBlocks)
{
	auto	   network = FakeDatagramNetwork::create();
	const auto start   = Clock::now();
	network->useVirtualTime(start);

	// 1000 datagrams of 4 bytes per second, and room for 10 that wait
	LinkProfile profile;
	profile.bandwidth  = 4000;
	profile.queueLimit = 10;
	network->setProfile(profile);

	auto sender	  = network->factory("10.0.0.1")({ipv4("0.0.0.0"), 0}, {});
	auto receiver = network->factory("10.0.0.2")({ipv4("0.0.0.0"), 4000}, {});
	ASSERT_TRUE(sender && receiver);

	for (uint32_t i = 0; i < 25; ++i)
		EXPECT_TRUE((*sender)->sendTo({ipv4("10.0.0.2"), 4000}, encode(i)).has_value()) << "A full queue drops silently";

	network->advanceTo(start + 5ms);
	EXPECT_EQ(drain(**receiver), (std::vector<uint32_t>{0, 1, 2, 3, 4})) << "One datagram per millisecond gets through";

	network->advanceTo(start + 1s);
	EXPECT_EQ(drain(**receiver).size(), 5u) << "Only what fitted into the queue was sent";

	profile.blockWhenFull = true;
	network->setProfile(profile);

	size_t accepted = 0;
	for (uint32_t i = 0; i < 25; ++i)
	{
		const auto sent = (*sender)->sendTo({ipv4("10.0.0.2"), 4000}, encode(i));
		accepted += sent.has_value() ? 1 : 0;

		if (!sent)
		{
			EXPECT_EQ(sent.error(), SocketError::WouldBlock);
		}
	}

	EXPECT_EQ(accepted, 10u);
	EXPECT_EQ(network->blockedSends(), 15u);
}


TEST(FakeDatagramNetwork, BindConflictWithoutReuse)
{
	auto network = FakeDatagramNetwork::create();
	auto first	 = network->factory("10.0.0.1")({ipv4("0.0.0.0"), 4000}, {});
	auto second	 = network->factory("10.0.0.1")({ipv4("0.0.0.0"), 4000}, {});

	ASSERT_TRUE(first.has_value());
	ASSERT_FALSE(second.has_value());
	EXPECT_EQ(second.error(), SocketError::AddressInUse);
}


TEST(LossyDatagramSocket, DropsApproximatelyTheConfiguredShare)
{
	auto		network = FakeDatagramNetwork::create();

	LossProfile profile;
	profile.dropRate = 0.25;

	auto sender		 = LossyDatagramSocket::wrap(network->factory("10.0.0.1"), profile)({ipv4("0.0.0.0"), 0}, {});
	auto receiver	 = network->factory("10.0.0.2")({ipv4("0.0.0.0"), 4000}, {});
	ASSERT_TRUE(sender && receiver);

	for (uint32_t i = 0; i < 1000; ++i)
		static_cast<void>((*sender)->sendTo({ipv4("10.0.0.2"), 4000}, encode(i)));

	const auto received = drain(**receiver).size();
	EXPECT_GT(received, 650u);
	EXPECT_LT(received, 850u);
}


TEST(LossyDatagramSocket, DuplicatesEveryDatagram)
{
	auto		network = FakeDatagramNetwork::create();

	LossProfile profile;
	profile.duplicateRate = 1.0;

	auto sender			  = LossyDatagramSocket::wrap(network->factory("10.0.0.1"), profile)({ipv4("0.0.0.0"), 0}, {});
	auto receiver		  = network->factory("10.0.0.2")({ipv4("0.0.0.0"), 4000}, {});
	ASSERT_TRUE(sender && receiver);

	for (uint32_t i = 0; i < 10; ++i)
		static_cast<void>((*sender)->sendTo({ipv4("10.0.0.2"), 4000}, encode(i)));

	EXPECT_EQ(drain(**receiver).size(), 20u);
}


TEST(LossyDatagramSocket, ReordersWithoutLosingDatagrams)
{
	auto		network = FakeDatagramNetwork::create();

	LossProfile profile;
	profile.reorderRate = 0.5;

	auto sender			= LossyDatagramSocket::wrap(network->factory("10.0.0.1"), profile)({ipv4("0.0.0.0"), 0}, {});
	auto receiver		= network->factory("10.0.0.2")({ipv4("0.0.0.0"), 4000}, {});
	ASSERT_TRUE(sender && receiver);

	for (uint32_t i = 0; i < 200; ++i)
		static_cast<void>((*sender)->sendTo({ipv4("10.0.0.2"), 4000}, encode(i)));

	const auto values = drain(**receiver);

	EXPECT_FALSE(std::is_sorted(values.begin(), values.end())) << "Some datagrams must arrive out of order";
	EXPECT_GE(values.size(), 199u) << "Reordering must not lose datagrams (at most the last one is still held back)";
	EXPECT_EQ(std::set<uint32_t>(values.begin(), values.end()).size(), values.size()) << "Reordering must not duplicate datagrams";
}

} // namespace SocketTests
