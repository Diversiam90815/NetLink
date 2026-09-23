#include <gtest/gtest.h>
#include "TestIp.h"
#include "ConnectionService/RoleNegotiation.h"

using namespace netlink;

namespace ConnectionTests
{

// Octet packing is covered by IPv4AddressTests; these cover the role decision itself.

// ---------------------------------------------------------------------------
// determineRole
// ---------------------------------------------------------------------------

TEST(DetermineRole, HigherIPBecomesAcceptor)
{
	EXPECT_EQ(determineRole(ipv4("192.168.1.200"), ipv4("192.168.1.100")), SessionRole::Acceptor)
		<< "The peer with the numerically higher IP (192.168.1.200) must become the Acceptor";
}


TEST(DetermineRole, LowerIPBecomesConnector)
{
	EXPECT_EQ(determineRole(ipv4("192.168.1.100"), ipv4("192.168.1.200")), SessionRole::Connector)
		<< "The peer with the numerically lower IP (192.168.1.100) must become the Connector";
}


TEST(DetermineRole, EqualIPsBecomesAcceptor)
{
	// Tie-breaking: >= means equal IPs → Acceptor
	EXPECT_EQ(determineRole(ipv4("10.0.0.1"), ipv4("10.0.0.1")), SessionRole::Acceptor) << "When both IPs are equal the >= condition must make the local peer the Acceptor";
}


TEST(DetermineRole, FirstOctetDominates)
{
	// 11.0.0.0 > 10.255.255.255 numerically — comparison must not be lexicographic
	EXPECT_EQ(determineRole(ipv4("11.0.0.0"), ipv4("10.255.255.255")), SessionRole::Acceptor) << "11.0.0.0 is numerically greater than 10.255.255.255 and must be the Acceptor";
	EXPECT_EQ(determineRole(ipv4("10.255.255.255"), ipv4("11.0.0.0")), SessionRole::Connector) << "10.255.255.255 is numerically less than 11.0.0.0 and must be the Connector";
}


TEST(DetermineRole, Symmetry)
{
	// For two distinct IPs, exactly one peer is Acceptor and the other is Connector
	auto r1 = determineRole(ipv4("192.168.0.10"), ipv4("192.168.0.20"));
	auto r2 = determineRole(ipv4("192.168.0.20"), ipv4("192.168.0.10"));
	EXPECT_NE(r1, r2) << "Swapping local and remote IPs must always produce opposite roles — one Acceptor and one Connector";
}

} // namespace ConnectionTests
