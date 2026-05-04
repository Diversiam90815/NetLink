/*
  ==============================================================================
	Module:         SignalingPacket
	Description:    Verification packet  ==============================================================================
*/


#pragma once

#include <string>
#include <cstdint>
#include <nlohmann/json.hpp>


namespace netlink
{

enum class SignalType : uint8_t
{
	ConnectRequest,
	ConnectAccept,
	ConnectDecline,
	Disconnect
};

struct SignalPacket
{
	SignalType	signalType{};
	std::string senderIP{};
	int			senderPort{0};
	int			senderSignalingPort{0};
	std::string displayName{};
};


inline void to_json(nlohmann::json &j, const SignalPacket &p)
{
	j = nlohmann::json{{"type", static_cast<int>(p.signalType)}, {"ip", p.senderIP}, {"port", p.senderPort}, {"sig", p.senderSignalingPort}, {"name", p.displayName}};
}

inline void from_json(const nlohmann::json &j, SignalPacket &p)
{
	p.signalType = static_cast<SignalType>(j.at("type").get<int>());
	j.at("ip").get_to(p.senderIP);
	j.at("port").get_to(p.senderPort);
	j.at("sig").get_to(p.senderSignalingPort);
	if (j.contains("name"))
		j.at("name").get_to(p.displayName);
}

} // namespace netlink
