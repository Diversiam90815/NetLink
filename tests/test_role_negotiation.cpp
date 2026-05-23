#include <gtest/gtest.h>
#include "ConnectionService/RoleNegotiation.h"

using namespace netlink;

// ---------------------------------------------------------------------------
// IPv4ToNumeric
// ---------------------------------------------------------------------------

TEST(IPv4ToNumeric, Loopback)
{
    EXPECT_EQ(IPv4ToNumeric("127.0.0.1"), 0x7F000001u)
        << "127.0.0.1 must pack to 0x7F000001 (127<<24 | 0<<16 | 0<<8 | 1)";
}

TEST(IPv4ToNumeric, AllZeros)
{
    EXPECT_EQ(IPv4ToNumeric("0.0.0.0"), 0u)
        << "0.0.0.0 must produce a numeric value of zero";
}

TEST(IPv4ToNumeric, AllOnes)
{
    EXPECT_EQ(IPv4ToNumeric("255.255.255.255"), 0xFFFFFFFFu)
        << "255.255.255.255 must fill all 32 bits without overflow or sign-extension";
}

TEST(IPv4ToNumeric, TypicalLAN)
{
    // 192=0xC0, 168=0xA8, 1=0x01, 100=0x64
    EXPECT_EQ(IPv4ToNumeric("192.168.1.100"), 0xC0A80164u)
        << "192.168.1.100 must pack to 0xC0A80164";
}

TEST(IPv4ToNumeric, AscendingOctets)
{
    // Guards against applying the shift to the wrong octet
    EXPECT_EQ(IPv4ToNumeric("1.2.3.4"), 0x01020304u)
        << "1.2.3.4 must pack to 0x01020304 — each octet must occupy its correct byte position";
}

// ---------------------------------------------------------------------------
// determineRole
// ---------------------------------------------------------------------------

TEST(DetermineRole, HigherIPBecomesAcceptor)
{
    EXPECT_EQ(determineRole("192.168.1.200", "192.168.1.100"), SessionRole::Acceptor)
        << "The peer with the numerically higher IP (192.168.1.200) must become the Acceptor";
}

TEST(DetermineRole, LowerIPBecomesConnector)
{
    EXPECT_EQ(determineRole("192.168.1.100", "192.168.1.200"), SessionRole::Connector)
        << "The peer with the numerically lower IP (192.168.1.100) must become the Connector";
}

TEST(DetermineRole, EqualIPsBecomesAcceptor)
{
    // Tie-breaking: >= means equal IPs → Acceptor
    EXPECT_EQ(determineRole("10.0.0.1", "10.0.0.1"), SessionRole::Acceptor)
        << "When both IPs are equal the >= condition must make the local peer the Acceptor";
}

TEST(DetermineRole, FirstOctetDominates)
{
    // 11.0.0.0 > 10.255.255.255 numerically — comparison must not be lexicographic
    EXPECT_EQ(determineRole("11.0.0.0", "10.255.255.255"), SessionRole::Acceptor)
        << "11.0.0.0 is numerically greater than 10.255.255.255 and must be the Acceptor";
    EXPECT_EQ(determineRole("10.255.255.255", "11.0.0.0"), SessionRole::Connector)
        << "10.255.255.255 is numerically less than 11.0.0.0 and must be the Connector";
}

TEST(DetermineRole, Symmetry)
{
    // For two distinct IPs, exactly one peer is Acceptor and the other is Connector
    auto r1 = determineRole("192.168.0.10", "192.168.0.20");
    auto r2 = determineRole("192.168.0.20", "192.168.0.10");
    EXPECT_NE(r1, r2)
        << "Swapping local and remote IPs must always produce opposite roles — one Acceptor and one Connector";
}
