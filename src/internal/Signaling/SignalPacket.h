/*
  ==============================================================================
	Module:         SignalingPacket
	Description:    Verification packet  ==============================================================================
*/


#pragma once

#include <string>
#include <cstdint>
#include <variant>
#include <nlohmann/json.hpp>


namespace netlink
{

namespace JSON_Serialization
{
constexpr auto SignalType	 = "type";
constexpr auto SenderIPv4	 = "IPv4";
constexpr auto SenderPort	 = "sigPort";
constexpr auto Payload		 = "payload";
constexpr auto ComputerName	 = "name";
constexpr auto DataPort		 = "dataPort";
constexpr auto Request		 = "request";
constexpr auto ConnectAnswer = "answer";
constexpr auto Secret		 = "secret";
constexpr auto Version		 = "version";
constexpr auto ReadyFlag	 = "ready";

} // namespace JSON_Serialization


enum class SignalType : uint8_t
{
	ConnectRequest,
	ConnectAnswer, // Answer for a connection request
	Disconnect,
	DataPort,
	ReadyFlag,
	ValidationRequest,
	SecretResponse,
	VersionResponse,
	ValidationHandshake,
};

// ---------------------------------------------------------------------------
// Per-signal payloads — only the fields relevant to that type
// ---------------------------------------------------------------------------

struct PayloadEmpty
{
};

struct PayloadConnectAnswer
{
	bool accepted{false};
};

struct PayloadDataPort
{
	int dataPort{0};
};

struct PayloadReadyFlag
{
	bool ready{false};
};

struct PayloadValidationRequest
{
	uint8_t request{0}; // RemoteRequest enum value
};

struct PayloadSecretResponse
{
	std::string secret{};
};

struct PayloadVersionResponse
{
	std::string version{};
};


// ---------------------------------------------------------------------------
// Envelope — sender identity is always present so the receiver can reply
// ---------------------------------------------------------------------------

struct SignalPacket
{
	SignalType																																					 signalType{};
	std::string																																					 senderName{};
	std::string																																					 senderIP{};
	int																																							 senderPort{0};

	// Type-specific payload
	std::variant<PayloadEmpty, PayloadConnectAnswer, PayloadDataPort, PayloadReadyFlag, PayloadValidationRequest, PayloadSecretResponse, PayloadVersionResponse> payload{
		PayloadEmpty{}};
};


// ---------------------------------------------------------------------------
// Serialization helpers
// ---------------------------------------------------------------------------

inline void to_json(nlohmann::json &j, const SignalPacket &p)
{
	j[JSON_Serialization::SignalType]	= static_cast<int>(p.signalType);
	j[JSON_Serialization::ComputerName] = p.senderName;
	j[JSON_Serialization::SenderIPv4]	= p.senderIP;
	j[JSON_Serialization::SenderPort]	= p.senderPort;

	std::visit(
		[&j](const auto &pl)
		{
			using T = std::decay_t<decltype(pl)>;
			if constexpr (std::is_same_v<T, PayloadConnectAnswer>)
				j[JSON_Serialization::Payload] = {{JSON_Serialization::ConnectAnswer, pl.accepted}};
			else if constexpr (std::is_same_v<T, PayloadReadyFlag>)
				j[JSON_Serialization::Payload] = {{JSON_Serialization::ReadyFlag, pl.ready}};
			else if constexpr (std::is_same_v<T, PayloadDataPort>)
				j[JSON_Serialization::Payload] = {{JSON_Serialization::DataPort, pl.dataPort}};
			else if constexpr (std::is_same_v<T, PayloadValidationRequest>)
				j[JSON_Serialization::Payload] = {{JSON_Serialization::Request, pl.request}};
			else if constexpr (std::is_same_v<T, PayloadSecretResponse>)
				j[JSON_Serialization::Payload] = {{JSON_Serialization::Secret, pl.secret}};
			else if constexpr (std::is_same_v<T, PayloadVersionResponse>)
				j[JSON_Serialization::Payload] = {{JSON_Serialization::Version, pl.version}};
			// PayloadEmpty: no payload field emitted
		},
		p.payload);
}

inline void from_json(const nlohmann::json &j, SignalPacket &p)
{
	p.signalType = static_cast<SignalType>(j.at(JSON_Serialization::SignalType).get<int>());
	j.at(JSON_Serialization::SenderIPv4).get_to(p.senderIP);
	j.at(JSON_Serialization::SenderPort).get_to(p.senderPort);
	j.at(JSON_Serialization::ComputerName).get_to(p.senderName);

	const auto &pl = j.contains(JSON_Serialization::Payload) ? j[JSON_Serialization::Payload] : nlohmann::json::object();

	switch (p.signalType)
	{
	case SignalType::ReadyFlag: p.payload = PayloadReadyFlag{pl.at(JSON_Serialization::ReadyFlag).get<bool>()}; break;
	case SignalType::ConnectAnswer: p.payload = PayloadConnectAnswer{pl.at(JSON_Serialization::ConnectAnswer).get<bool>()}; break;
	case SignalType::DataPort: p.payload = PayloadDataPort{pl.at(JSON_Serialization::DataPort).get<int>()}; break;
	case SignalType::ValidationRequest: p.payload = PayloadValidationRequest{pl.at(JSON_Serialization::Request).get<uint8_t>()}; break;
	case SignalType::SecretResponse: p.payload = PayloadSecretResponse{pl.at(JSON_Serialization::Secret).get<std::string>()}; break;
	case SignalType::VersionResponse: p.payload = PayloadVersionResponse{pl.at(JSON_Serialization::Version).get<std::string>()}; break;
	default: p.payload = PayloadEmpty{}; break;
	}
}

} // namespace netlink
