#include <gtest/gtest.h>
#include "ConnectionService/RoleNegotiation.h"

using namespace netlink;

// ---------------------------------------------------------------------------
// IPv4ToNumeric
// ---------------------------------------------------------------------------

TEST(IPv4ToNumeric, Loopback)
{
    EXPECT_EQ(IPv4ToNumeric("127.0.0.1"), 0x7F000001u);
}

TEST(IPv4ToNumeric, AllZeros)
{
    EXPECT_EQ(IPv4ToNumeric("0.0.0.0"), 0u);
}

TEST(IPv4ToNumeric, AllOnes)
{
    EXPECT_EQ(IPv4ToNumeric("255.255.255.255"), 0xFFFFFFFFu);
}

TEST(IPv4ToNumeric, TypicalLAN)
{
    // 192=0xC0, 168=0xA8, 1=0x01, 100=0x64
    EXPECT_EQ(IPv4ToNumeric("192.168.1.100"), 0xC0A80164u);
}

TEST(IPv4ToNumeric, AscendingOctets)
{
    // Guards against applying the shift to the wrong octet
    EXPECT_EQ(IPv4ToNumeric("1.2.3.4"), 0x01020304u);
}

// ---------------------------------------------------------------------------
// determineRole
// ---------------------------------------------------------------------------

TEST(DetermineRole, HigherIPBecomesAcceptor)
{
    EXPECT_EQ(determineRole("192.168.1.200", "192.168.1.100"), SessionRole::Acceptor);
}

TEST(DetermineRole, LowerIPBecomesConnector)
{
    EXPECT_EQ(determineRole("192.168.1.100", "192.168.1.200"), SessionRole::Connector);
}

TEST(DetermineRole, EqualIPsBecomesAcceptor)
{
    // Tie-breaking: >= means equal IPs → Acceptor
    EXPECT_EQ(determineRole("10.0.0.1", "10.0.0.1"), SessionRole::Acceptor);
}

TEST(DetermineRole, FirstOctetDominates)
{
    // 11.0.0.0 > 10.255.255.255 numerically — not lexicographic
    EXPECT_EQ(determineRole("11.0.0.0", "10.255.255.255"), SessionRole::Acceptor);
    EXPECT_EQ(determineRole("10.255.255.255", "11.0.0.0"), SessionRole::Connector);
}

TEST(DetermineRole, Symmetry)
{
    // For two distinct IPs, exactly one peer is Acceptor and the other is Connector
    auto r1 = determineRole("192.168.0.10", "192.168.0.20");
    auto r2 = determineRole("192.168.0.20", "192.168.0.10");
    EXPECT_NE(r1, r2);
}
