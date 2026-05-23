#include <gtest/gtest.h>
#include "Network/NetworkInformation.h"

using namespace netlink;

namespace
{
// Construct a fully-populated adapter for convenience
NetworkAdapterInternal makeAdapter(const std::string        &name   = "Ethernet",
                                   const std::string        &ip     = "192.168.1.10",
                                   const std::string        &subnet = "255.255.255.0",
                                   int                       id     = 1,
                                   bool                      isDefault = false,
                                   AdapterTypes              type   = AdapterTypes::Ethernet,
                                   AdapterPriorityInternal   prio   = AdapterPriorityInternal::Available)
{
    return NetworkAdapterInternal(name, "MyNetwork", ip, subnet, id, isDefault, type, prio);
}
} // namespace

// ---------------------------------------------------------------------------
// isValid
// ---------------------------------------------------------------------------

TEST(NetworkAdapterInternal, IsValidAllFieldsPresent)
{
    auto a = makeAdapter("Ethernet", "192.168.1.10", "255.255.255.0", 1);
    EXPECT_TRUE(a.isValid());
}

TEST(NetworkAdapterInternal, IsValidEmptyName)
{
    auto a = makeAdapter("", "192.168.1.10", "255.255.255.0", 1);
    EXPECT_FALSE(a.isValid());
}

TEST(NetworkAdapterInternal, IsValidEmptyIP)
{
    auto a = makeAdapter("Ethernet", "", "255.255.255.0", 1);
    EXPECT_FALSE(a.isValid());
}

TEST(NetworkAdapterInternal, IsValidZeroID)
{
    auto a = makeAdapter("Ethernet", "192.168.1.10", "255.255.255.0", 0);
    EXPECT_FALSE(a.isValid());
}

// ---------------------------------------------------------------------------
// filterSubnetMask
// ---------------------------------------------------------------------------

TEST(NetworkAdapterInternal, FilterSubnetMaskTrue)
{
    auto a = makeAdapter("eth0", "10.0.0.1", "255.255.255.0", 1);
    EXPECT_TRUE(a.filterSubnetMask());
}

TEST(NetworkAdapterInternal, FilterSubnetMaskFalseClassB)
{
    auto a = makeAdapter("eth0", "10.0.0.1", "255.255.0.0", 1);
    EXPECT_FALSE(a.filterSubnetMask());
}

TEST(NetworkAdapterInternal, FilterSubnetMaskFalseClassA)
{
    auto a = makeAdapter("eth0", "10.0.0.1", "255.0.0.0", 1);
    EXPECT_FALSE(a.filterSubnetMask());
}

// ---------------------------------------------------------------------------
// Eligible flag set by constructor
// ---------------------------------------------------------------------------

TEST(NetworkAdapterInternal, ConstructorSetsEligibleWhenSubnetMatches)
{
    auto a = makeAdapter("eth0", "192.168.1.1", "255.255.255.0", 1);
    EXPECT_TRUE(a.Eligible);
}

TEST(NetworkAdapterInternal, ConstructorSetsEligibleFalseWhenSubnetMismatches)
{
    auto a = makeAdapter("eth0", "192.168.1.1", "255.255.0.0", 1);
    EXPECT_FALSE(a.Eligible);
}

// ---------------------------------------------------------------------------
// operator== / operator!=  (compare by AdapterName AND Subnet)
// ---------------------------------------------------------------------------

TEST(NetworkAdapterInternal, EqualityByNameAndSubnet)
{
    auto a = makeAdapter("Ethernet", "192.168.1.10", "255.255.255.0", 1);
    auto b = makeAdapter("Ethernet", "10.0.0.5",     "255.255.255.0", 2); // different IP + ID
    EXPECT_EQ(a, b);
}

TEST(NetworkAdapterInternal, InequalityDifferentName)
{
    auto a = makeAdapter("Ethernet", "192.168.1.10", "255.255.255.0", 1);
    auto b = makeAdapter("WiFi",     "192.168.1.10", "255.255.255.0", 1);
    EXPECT_NE(a, b);
}

TEST(NetworkAdapterInternal, InequalityDifferentSubnet)
{
    auto a = makeAdapter("Ethernet", "192.168.1.10", "255.255.255.0", 1);
    auto b = makeAdapter("Ethernet", "192.168.1.10", "255.255.0.0",   1);
    EXPECT_NE(a, b);
}

// ---------------------------------------------------------------------------
// Enum values — guard against reordering
// ---------------------------------------------------------------------------

TEST(NetworkAdapterInternal, AdapterTypeEnumValues)
{
    EXPECT_EQ(static_cast<int>(AdapterTypes::Ethernet), 1);
    EXPECT_EQ(static_cast<int>(AdapterTypes::WiFi),     2);
    EXPECT_EQ(static_cast<int>(AdapterTypes::Loopback), 3);
    EXPECT_EQ(static_cast<int>(AdapterTypes::Virtual),  4);
    EXPECT_EQ(static_cast<int>(AdapterTypes::Other),    5);
}

TEST(NetworkAdapterInternal, PriorityEnumValues)
{
    EXPECT_EQ(static_cast<int>(AdapterPriorityInternal::Suppressed), 1);
    EXPECT_EQ(static_cast<int>(AdapterPriorityInternal::Available),  2);
    EXPECT_EQ(static_cast<int>(AdapterPriorityInternal::Preferred),  3);
}
