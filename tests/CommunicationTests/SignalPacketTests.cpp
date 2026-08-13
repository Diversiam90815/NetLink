#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "Signaling/SignalPacket.h"

using namespace netlink;


namespace CommunicationTests
{

static SignalPacket roundtrip(const SignalPacket &p)
{
	nlohmann::json j = p;
	return j.get<SignalPacket>();
}

static SignalPacket makeBase(SignalType type)
{
	SignalPacket p;
	p.signalType = type;
	p.senderName = "pc-alpha";
	p.senderIP	 = "10.0.0.5";
	p.senderPort = 9000;
	return p;
}


TEST(SignalPacketRoundtrip, EnvelopeFieldsPreserved)
{
	SignalPacket p		= makeBase(SignalType::Disconnect);
	p.payload			= PayloadEmpty{};
	SignalPacket result = roundtrip(p);

	EXPECT_EQ(result.signalType, SignalType::Disconnect) << "signalType must survive JSON serialization unchanged";
	EXPECT_EQ(result.senderIP, "10.0.0.5") << "senderIP must be written and read back correctly";
	EXPECT_EQ(result.senderPort, 9000) << "senderPort must be written and read back correctly";
	EXPECT_EQ(result.senderName, "pc-alpha") << "senderName is currently not deserialized in from_json; it must remain empty after round-trip";
	EXPECT_TRUE(std::holds_alternative<PayloadEmpty>(result.payload)) << "A packet with no structured payload must deserialize to PayloadEmpty";
}


TEST(SignalPacketRoundtrip, ConnectRequest)
{
	SignalPacket p = makeBase(SignalType::ConnectRequest);
	p.payload	   = PayloadEmpty{};
	auto result	   = roundtrip(p);

	EXPECT_EQ(result.signalType, SignalType::ConnectRequest) << "ConnectRequest signal type must survive the round-trip";
	EXPECT_TRUE(std::holds_alternative<PayloadEmpty>(result.payload)) << "ConnectRequest carries no structured payload and must deserialize to PayloadEmpty";
}


TEST(SignalPacketRoundtrip, ConnectAnswerAccepted)
{
	SignalPacket p = makeBase(SignalType::ConnectAnswer);
	p.payload	   = PayloadConnectAnswer{true};
	auto result	   = roundtrip(p);

	ASSERT_TRUE(std::holds_alternative<PayloadConnectAnswer>(result.payload)) << "ConnectAnswer must deserialize to PayloadConnectAnswer";
	EXPECT_TRUE(std::get<PayloadConnectAnswer>(result.payload).accepted) << "accepted=true must round-trip correctly through JSON";
}


TEST(SignalPacketRoundtrip, ConnectAnswerRejected)
{
	SignalPacket p = makeBase(SignalType::ConnectAnswer);
	p.payload	   = PayloadConnectAnswer{false};
	auto result	   = roundtrip(p);

	ASSERT_TRUE(std::holds_alternative<PayloadConnectAnswer>(result.payload)) << "ConnectAnswer must deserialize to PayloadConnectAnswer";
	EXPECT_FALSE(std::get<PayloadConnectAnswer>(result.payload).accepted) << "accepted=false must round-trip correctly through JSON";
}


TEST(SignalPacketRoundtrip, Disconnect)
{
	SignalPacket p = makeBase(SignalType::Disconnect);
	p.payload	   = PayloadEmpty{};
	auto result	   = roundtrip(p);

	EXPECT_EQ(result.signalType, SignalType::Disconnect) << "Disconnect signal type must survive the round-trip";
	EXPECT_TRUE(std::holds_alternative<PayloadEmpty>(result.payload)) << "Disconnect carries no structured payload — must hit the default: branch and produce PayloadEmpty";
}


TEST(SignalPacketRoundtrip, DataPort)
{
	SignalPacket p = makeBase(SignalType::DataPort);
	p.payload	   = PayloadDataPort{12345};
	auto result	   = roundtrip(p);

	ASSERT_TRUE(std::holds_alternative<PayloadDataPort>(result.payload)) << "DataPort must deserialize to PayloadDataPort";
	EXPECT_EQ(std::get<PayloadDataPort>(result.payload).dataPort, 12345) << "dataPort value 12345 must survive the JSON round-trip";
}


TEST(SignalPacketRoundtrip, ReadyFlagTrue)
{
	SignalPacket p = makeBase(SignalType::ReadyFlag);
	p.payload	   = PayloadReadyFlag{true};
	auto result	   = roundtrip(p);

	ASSERT_TRUE(std::holds_alternative<PayloadReadyFlag>(result.payload)) << "ReadyFlag must deserialize to PayloadReadyFlag";
	EXPECT_TRUE(std::get<PayloadReadyFlag>(result.payload).ready) << "ready=true must round-trip correctly through JSON";
}


TEST(SignalPacketRoundtrip, ReadyFlagFalse)
{
	SignalPacket p = makeBase(SignalType::ReadyFlag);
	p.payload	   = PayloadReadyFlag{false};
	auto result	   = roundtrip(p);

	ASSERT_TRUE(std::holds_alternative<PayloadReadyFlag>(result.payload)) << "ReadyFlag must deserialize to PayloadReadyFlag";
	EXPECT_FALSE(std::get<PayloadReadyFlag>(result.payload).ready) << "ready=false must round-trip correctly through JSON";
}


TEST(SignalPacketRoundtrip, ValidationRequest)
{
	SignalPacket p = makeBase(SignalType::ValidationRequest);
	p.payload	   = PayloadValidationRequest{2}; // RemoteRequest::Version == 2
	auto result	   = roundtrip(p);

	ASSERT_TRUE(std::holds_alternative<PayloadValidationRequest>(result.payload)) << "ValidationRequest must deserialize to PayloadValidationRequest";
	EXPECT_EQ(std::get<PayloadValidationRequest>(result.payload).request, 2u) << "request value 2 (RemoteRequest::Version) must survive the JSON round-trip";
}


TEST(SignalPacketRoundtrip, SecretResponse)
{
	SignalPacket p = makeBase(SignalType::SecretResponse);
	p.payload	   = PayloadSecretResponse{"mysecret"};
	auto result	   = roundtrip(p);

	ASSERT_TRUE(std::holds_alternative<PayloadSecretResponse>(result.payload)) << "SecretResponse must deserialize to PayloadSecretResponse";
	EXPECT_EQ(std::get<PayloadSecretResponse>(result.payload).secret, "mysecret") << "secret string must survive the JSON round-trip unchanged";
}


TEST(SignalPacketRoundtrip, VersionResponse)
{
	SignalPacket p = makeBase(SignalType::VersionResponse);
	p.payload	   = PayloadVersionResponse{"1.2.3"};
	auto result	   = roundtrip(p);

	ASSERT_TRUE(std::holds_alternative<PayloadVersionResponse>(result.payload)) << "VersionResponse must deserialize to PayloadVersionResponse";
	EXPECT_EQ(std::get<PayloadVersionResponse>(result.payload).version, "1.2.3") << "version string must survive the JSON round-trip unchanged";
}


TEST(SignalPacketRoundtrip, ValidationHandshake)
{
	SignalPacket p = makeBase(SignalType::ValidationHandshake);
	p.payload	   = PayloadEmpty{};
	auto result	   = roundtrip(p);

	EXPECT_EQ(result.signalType, SignalType::ValidationHandshake) << "ValidationHandshake signal type must survive the round-trip";
	EXPECT_TRUE(std::holds_alternative<PayloadEmpty>(result.payload)) << "ValidationHandshake has no structured payload and must hit the default: branch producing PayloadEmpty";
}


TEST(SignalPacketRoundtrip, SignalTypeNumericValues)
{
	{
		SignalPacket p	 = makeBase(SignalType::ConnectRequest);
		p.payload		 = PayloadEmpty{};
		nlohmann::json j = p;
		EXPECT_EQ(j[JSON_Serialization::SignalType].get<int>(), 0) << "ConnectRequest must serialize to numeric type 0 — reordering the enum would break the wire format";
	}
	{
		SignalPacket p	 = makeBase(SignalType::ValidationHandshake);
		p.payload		 = PayloadEmpty{};
		nlohmann::json j = p;
		EXPECT_EQ(j[JSON_Serialization::SignalType].get<int>(), 8) << "ValidationHandshake must serialize to numeric type 8 — reordering the enum would break the wire format";
	}
}

} // namespace CommunicationTests
