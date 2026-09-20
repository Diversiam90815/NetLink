/*
==============================================================================
	Module:         RoleNegotiation
	Description:    Deterministic TCP role assignment based on IP comparison
  ==============================================================================
*/

#pragma once

#include "Socket/IPv4Address.h"


namespace netlink
{

enum class SessionRole
{
	Acceptor, // Listens for incoming TCP
	Connector // Initiates TCP connection
};


// Higher numeric IP value becomes the acceptor. Both peers run this on the same
// pair of addresses and so arrive at complementary roles.
inline SessionRole determineRole(const net::IPv4Address &localIP, const net::IPv4Address &remoteIP)
{
	return localIP >= remoteIP ? SessionRole::Acceptor : SessionRole::Connector;
}

} // namespace netlink
