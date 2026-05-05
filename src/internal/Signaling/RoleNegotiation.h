/*
==============================================================================
	Module:         RoleNegotiation
	Description:    Deterministic TCP role assignment based on IP comparison
  ==============================================================================
*/

#pragma once

#include <string>
#include <cstdint>
#include <sstream>


namespace netlink
{

enum class SessionRole
{
	Acceptor, // Listens for incoming TCP
	Connector // Initiates TCP connection
};


inline uint32_t IPv4ToNumeric(const std::string &ipv4)
{
	uint32_t		   result = 0;
	int				   shift  = 24;
	std::istringstream stream(ipv4);
	std::string		   token;

	while (std::getline(stream, token, "."))
	{
		result = static_cast<uint32_t>(std::stoul(token) << shift);
		shift -= 8;
	}

	return result;
}


inline SessionRole determineRole(const std::string &localIP, const std::string &remoteIP)
{
	uint32_t localVal  = IPv4ToNumeric(localIP);
	uint32_t remoteVal = IPv4ToNumeric(remoteIP);

	// Higher numeric IP value becomes the acceptor
	return (localVal >= remoteVal) ? SessionRole::Acceptor : SessionRole::Connector;
}

} // namespace netlink
