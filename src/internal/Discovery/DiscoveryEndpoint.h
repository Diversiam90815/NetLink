/*
  ==============================================================================
	Module:         DiscoveryEndpoint
	Description:    Internal endpoint used for network discovery
  ==============================================================================
*/

#pragma once

#include <string>
#include <nlohmann/json.hpp>


// @brief		Internal endpoint representation for discovery packets
struct DiscoveryEndpoint
{
	std::string IPAddress{};
	int			port{0};
	int			signalingPort{0};
	std::string displayName{};

	bool		operator==(const DiscoveryEndpoint &other) const { return IPAddress == other.IPAddress && port == other.port; }

	bool		isValid() const { return !IPAddress.empty() && port != 0 && signalingPort != 0; }
	bool		isEmpty() const { return IPAddress.empty() && port == 0 && signalingPort == 0; }
};


inline void to_json(nlohmann::json &j, const DiscoveryEndpoint &ep)
{
	j = nlohmann::json{{"ip", ep.IPAddress}, {"port", ep.port}, {"sig", ep.signalingPort}, {"name", ep.displayName}};
}

inline void from_json(const nlohmann::json &j, DiscoveryEndpoint &ep)
{
	j.at("ip").get_to(ep.IPAddress);
	j.at("port").get_to(ep.port);
	j.at("sig").get_to(ep.signalingPort);
	if (j.contains("name"))
		j.at("name").get_to(ep.displayName);
}
