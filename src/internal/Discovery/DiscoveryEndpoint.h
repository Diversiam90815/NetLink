/*
  ==============================================================================
	Module:         DiscoveryEndpoint
	Description:    Internal endpoint used for network discovery
  ==============================================================================
*/

#pragma once

#include <string>
#include <nlohmann/json.hpp>

#include "Socket/IPv4Address.h"


// @brief		Internal endpoint representation for discovery packets
struct DiscoveryEndpoint
{
	netlink::net::IPv4Address IPAddress{};
	int						  port{0};
	std::string				  displayName{};

	bool					  operator==(const DiscoveryEndpoint &other) const { return IPAddress == other.IPAddress && port == other.port; }

	bool					  isValid() const { return !IPAddress.isUnspecified() && port != 0; }
	bool					  isEmpty() const { return IPAddress.isUnspecified() && port == 0; }
};


inline void to_json(nlohmann::json &j, const DiscoveryEndpoint &ep)
{
	j = nlohmann::json{{"ip", ep.IPAddress.toString()}, {"port", ep.port}, {"name", ep.displayName}};
}

inline void from_json(const nlohmann::json &j, DiscoveryEndpoint &ep)
{
	// An unparsable address leaves IPAddress unspecified, so isValid() rejects the endpoint
	if (auto ip = netlink::net::IPv4Address::parse(j.at("ip").get<std::string>()); ip.has_value())
		ep.IPAddress = *ip;
	j.at("port").get_to(ep.port);
	if (j.contains("name"))
		j.at("name").get_to(ep.displayName);
}
