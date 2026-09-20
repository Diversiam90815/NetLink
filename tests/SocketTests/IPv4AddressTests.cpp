#include <gtest/gtest.h>

#include "Socket/IPv4Address.h"

using netlink::net::IPv4Address;
using netlink::net::sameSubnet;
using netlink::net::subnetBroadcast;

namespace SocketTests
{

// ---------------------------------------------------------------------------
// parse: well formed input
// ---------------------------------------------------------------------------

TEST(IPv4Address, ParsesDottedQuadIntoPackedOctets)
{
	// 192=0xC0, 168=0xA8, 1=0x01, 100=0x64 — guards against shifting the wrong octet
	EXPECT_EQ(IPv4Address::parse("192.168.1.100")->toHostOrder(), 0xC0A80164u);
	EXPECT_EQ(IPv4Address::parse("1.2.3.4")->toHostOrder(), 0x01020304u) << "Each octet must occupy its own byte position";
	EXPECT_EQ(IPv4Address::parse("127.0.0.1")->toHostOrder(), 0x7F000001u);
}


TEST(IPv4Address, ParsesBoundaryValues)
{
	EXPECT_EQ(IPv4Address::parse("0.0.0.0")->toHostOrder(), 0u);
	EXPECT_EQ(IPv4Address::parse("255.255.255.255")->toHostOrder(), 0xFFFFFFFFu) << "All ones must fill 32 bits without overflow or sign extension";
}


TEST(IPv4Address, SurvivesStringRoundTrip)
{
	for (const char *text : {"0.0.0.0", "10.0.0.5", "192.168.1.100", "255.255.255.255"})
		EXPECT_EQ(IPv4Address::parse(text)->toString(), text) << "parse() -> toString() must reproduce the input exactly";
}


// ---------------------------------------------------------------------------
// parse: malformed input must be rejected rather than throwing or truncating
// ---------------------------------------------------------------------------

TEST(IPv4Address, RejectsMalformedInput)
{
	// Each of these previously reached std::stoul, which threw, or shifted by a negative amount
	const char *malformed[] = {"", "1.2.3", "1.2.3.4.5", "1.2.3.", ".1.2.3", "1..2.3", "999.0.0.1", "256.1.1.1", "a.b.c.d", "1.2.3.-4", "1.2.3.4x", "1111.1.1.1"};

	for (const char *text : malformed)
		EXPECT_FALSE(IPv4Address::parse(text).has_value()) << "'" << text << "' must be rejected";
}


TEST(IPv4Address, RejectsSurroundingWhitespace)
{
	EXPECT_FALSE(IPv4Address::parse(" 1.2.3.4").has_value());
	EXPECT_FALSE(IPv4Address::parse("1.2.3.4 ").has_value());
}


TEST(IPv4Address, RejectsLeadingZerosButAcceptsBareZero)
{
	// Leading zeros are ambiguously octal, so they are refused outright
	EXPECT_FALSE(IPv4Address::parse("01.02.03.04").has_value());
	EXPECT_FALSE(IPv4Address::parse("1.2.3.04").has_value());
	EXPECT_TRUE(IPv4Address::parse("0.0.0.0").has_value()) << "A single zero octet is still valid";
}


// ---------------------------------------------------------------------------
// Classification and ordering
// ---------------------------------------------------------------------------

TEST(IPv4Address, DefaultConstructedIsUnspecified)
{
	EXPECT_TRUE(IPv4Address().isUnspecified());
	EXPECT_EQ(IPv4Address().toString(), "0.0.0.0");
}


TEST(IPv4Address, ClassifiesSpecialAddresses)
{
	EXPECT_TRUE(IPv4Address::parse("127.0.0.1")->isLoopback());
	EXPECT_TRUE(IPv4Address::parse("127.255.255.254")->isLoopback()) << "The whole 127/8 block is loopback";
	EXPECT_FALSE(IPv4Address::parse("128.0.0.1")->isLoopback());
	EXPECT_TRUE(IPv4Address::parse("255.255.255.255")->isBroadcast());
	EXPECT_FALSE(IPv4Address::parse("255.255.255.254")->isBroadcast());
}


TEST(IPv4Address, OrdersNumerically)
{
	// Role negotiation depends on this ordering being the numeric one
	EXPECT_GT(*IPv4Address::parse("192.168.1.10"), *IPv4Address::parse("192.168.1.9"));
	EXPECT_LT(*IPv4Address::parse("10.0.0.1"), *IPv4Address::parse("192.168.0.1"));
	EXPECT_EQ(*IPv4Address::parse("1.2.3.4"), *IPv4Address::parse("1.2.3.4"));
}


// ---------------------------------------------------------------------------
// Subnet helpers
// ---------------------------------------------------------------------------

TEST(IPv4Address, ComputesSubnetDirectedBroadcast)
{
	EXPECT_EQ(subnetBroadcast(*IPv4Address::parse("192.168.1.7"), *IPv4Address::parse("255.255.255.0")), *IPv4Address::parse("192.168.1.255"));
	EXPECT_EQ(subnetBroadcast(*IPv4Address::parse("10.1.2.3"), *IPv4Address::parse("255.255.0.0")), *IPv4Address::parse("10.1.255.255"));
	EXPECT_EQ(subnetBroadcast(*IPv4Address::parse("172.16.5.9"), *IPv4Address::parse("255.240.0.0")), *IPv4Address::parse("172.31.255.255"))
		<< "Non /8, /16 or /24 masks must work too";
}


TEST(IPv4Address, DetectsSameSubnet)
{
	const auto mask = *IPv4Address::parse("255.255.255.0");

	EXPECT_TRUE(sameSubnet(*IPv4Address::parse("192.168.1.7"), *IPv4Address::parse("192.168.1.200"), mask));
	EXPECT_FALSE(sameSubnet(*IPv4Address::parse("192.168.1.7"), *IPv4Address::parse("192.168.2.7"), mask));
}

} // namespace SocketTests
