#include <gtest/gtest.h>
#include "Network/NetworkInformation.h"

using namespace netlink;


namespace NetworkTests
{

static NetworkAdapterInternal makeAdapter(const std::string		 &name		= "Ethernet",
										  const std::string		 &ip		= "192.168.1.10",
										  const std::string		 &subnet	= "255.255.255.0",
										  int					  id		= 1,
										  bool					  isDefault = false,
										  AdapterTypes			  type		= AdapterTypes::Ethernet,
										  AdapterPriorityInternal prio		= AdapterPriorityInternal::Available)
{
	return NetworkAdapterInternal(name, "MyNetwork", ip, subnet, id, isDefault, type, prio);
}


// ---------------------------------------------------------------------------
// isValid
// ---------------------------------------------------------------------------

TEST(NetworkAdapterInternal, IsValidAllFieldsPresent)
{
	auto a = makeAdapter("Ethernet", "192.168.1.10", "255.255.255.0", 1);
	EXPECT_TRUE(a.isValid()) << "An adapter with a non-empty name, non-empty IP, and non-zero ID must be valid";
}


TEST(NetworkAdapterInternal, IsValidEmptyName)
{
	auto a = makeAdapter("", "192.168.1.10", "255.255.255.0", 1);
	EXPECT_FALSE(a.isValid()) << "An adapter with an empty AdapterName must not be valid";
}


TEST(NetworkAdapterInternal, IsValidEmptyIP)
{
	auto a = makeAdapter("Ethernet", "", "255.255.255.0", 1);
	EXPECT_FALSE(a.isValid()) << "An adapter with an empty IPv4 address must not be valid";
}


TEST(NetworkAdapterInternal, IsValidZeroID)
{
	auto a = makeAdapter("Ethernet", "192.168.1.10", "255.255.255.0", 0);
	EXPECT_FALSE(a.isValid()) << "An adapter with ID 0 must not be valid — 0 is the unassigned sentinel value";
}


// ---------------------------------------------------------------------------
// filterSubnetMask
// ---------------------------------------------------------------------------

TEST(NetworkAdapterInternal, FilterSubnetMaskAcceptsAnyContiguousNetmask)
{
	// The filter used to accept only /24, which excluded every other LAN layout. The
	// mask is now load bearing (discovery derives the subnet broadcast from it), so
	// what matters is that it is a well formed netmask.
	EXPECT_TRUE(makeAdapter("eth0", "10.0.0.1", "255.255.255.0", 1).filterSubnetMask()) << "/24 must pass";
	EXPECT_TRUE(makeAdapter("eth0", "10.0.0.1", "255.255.0.0", 1).filterSubnetMask()) << "/16 must pass";
	EXPECT_TRUE(makeAdapter("eth0", "10.0.0.1", "255.0.0.0", 1).filterSubnetMask()) << "/8 must pass";
	EXPECT_TRUE(makeAdapter("eth0", "10.0.0.1", "255.255.240.0", 1).filterSubnetMask()) << "/20 must pass";
}


TEST(NetworkAdapterInternal, FilterSubnetMaskRejectsMalformedMasks)
{
	EXPECT_FALSE(makeAdapter("eth0", "10.0.0.1", "255.0.255.0", 1).filterSubnetMask()) << "A non-contiguous mask is not a netmask";
	EXPECT_FALSE(makeAdapter("eth0", "10.0.0.1", "0.0.0.0", 1).filterSubnetMask()) << "An all-zero mask selects nothing and is unusable";
	EXPECT_FALSE(makeAdapter("eth0", "10.0.0.1", "not-a-mask", 1).filterSubnetMask()) << "Unparsable text is not a netmask";
	EXPECT_FALSE(makeAdapter("eth0", "10.0.0.1", "", 1).filterSubnetMask()) << "An adapter without a reported mask is not eligible";
}


// ---------------------------------------------------------------------------
// Eligible flag set by constructor
// ---------------------------------------------------------------------------

TEST(NetworkAdapterInternal, ConstructorSetsEligibleWhenSubnetMatches)
{
	auto a = makeAdapter("eth0", "192.168.1.1", "255.255.255.0", 1);
	EXPECT_TRUE(a.Eligible) << "The constructor must set Eligible=true when filterSubnetMask() accepts the mask";
}


TEST(NetworkAdapterInternal, ConstructorSetsEligibleFalseWhenSubnetIsUnusable)
{
	auto a = makeAdapter("eth0", "192.168.1.1", "255.0.255.0", 1);
	EXPECT_FALSE(a.Eligible) << "The constructor must set Eligible=false when filterSubnetMask() rejects the mask";
}


// ---------------------------------------------------------------------------
// operator== / operator!=  (compare by AdapterName AND Subnet)
// ---------------------------------------------------------------------------

TEST(NetworkAdapterInternal, EqualityByNameAndSubnet)
{
	auto a = makeAdapter("Ethernet", "192.168.1.10", "255.255.255.0", 1);
	auto b = makeAdapter("Ethernet", "10.0.0.5", "255.255.255.0", 2); // different IP and ID
	EXPECT_EQ(a, b) << "Two adapters with the same AdapterName and Subnet must be equal regardless of IP or ID";
}


TEST(NetworkAdapterInternal, InequalityDifferentName)
{
	auto a = makeAdapter("Ethernet", "192.168.1.10", "255.255.255.0", 1);
	auto b = makeAdapter("WiFi", "192.168.1.10", "255.255.255.0", 1);
	EXPECT_NE(a, b) << "Adapters with different AdapterNames must not be equal";
}


TEST(NetworkAdapterInternal, InequalityDifferentSubnet)
{
	auto a = makeAdapter("Ethernet", "192.168.1.10", "255.255.255.0", 1);
	auto b = makeAdapter("Ethernet", "192.168.1.10", "255.255.0.0", 1);
	EXPECT_NE(a, b) << "Adapters with the same name but different Subnet masks must not be equal";
}


// ---------------------------------------------------------------------------
// Enum values — guard against reordering
// ---------------------------------------------------------------------------

TEST(NetworkAdapterInternal, AdapterTypeEnumValues)
{
	EXPECT_EQ(static_cast<int>(AdapterTypes::Ethernet), 1) << "AdapterTypes::Ethernet must have numeric value 1";
	EXPECT_EQ(static_cast<int>(AdapterTypes::WiFi), 2) << "AdapterTypes::WiFi must have numeric value 2";
	EXPECT_EQ(static_cast<int>(AdapterTypes::Loopback), 3) << "AdapterTypes::Loopback must have numeric value 3";
	EXPECT_EQ(static_cast<int>(AdapterTypes::Virtual), 4) << "AdapterTypes::Virtual must have numeric value 4";
	EXPECT_EQ(static_cast<int>(AdapterTypes::Other), 5) << "AdapterTypes::Other must have numeric value 5";
}


TEST(NetworkAdapterInternal, PriorityEnumValues)
{
	EXPECT_EQ(static_cast<int>(AdapterPriorityInternal::Suppressed), 1) << "AdapterPriorityInternal::Suppressed must have numeric value 1";
	EXPECT_EQ(static_cast<int>(AdapterPriorityInternal::Available), 2) << "AdapterPriorityInternal::Available must have numeric value 2";
	EXPECT_EQ(static_cast<int>(AdapterPriorityInternal::Preferred), 3) << "AdapterPriorityInternal::Preferred must have numeric value 3";
}

} // namespace NetworkTests
