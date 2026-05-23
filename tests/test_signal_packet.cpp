#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "Signaling/SignalPacket.h"

using namespace netlink;

namespace
{
SignalPacket roundtrip(const SignalPacket &p)
{
    nlohmann::json j = p;
    return j.get<SignalPacket>();
}

SignalPacket makeBase(SignalType type)
{
    SignalPacket p;
    p.signalType = type;
    p.senderName = "pc-alpha";
    p.senderIP   = "10.0.0.5";
    p.senderPort = 9000;
    return p;
}
} // namespace

// ---------------------------------------------------------------------------
// Envelope fields
// ---------------------------------------------------------------------------

TEST(SignalPacketRoundtrip, EnvelopeFieldsPreserved)
{
    SignalPacket p      = makeBase(SignalType::Disconnect);
    p.payload           = PayloadEmpty{};
    SignalPacket result  = roundtrip(p);

    EXPECT_EQ(result.signalType, SignalType::Disconnect);
    EXPECT_EQ(result.senderIP, "10.0.0.5");
    EXPECT_EQ(result.senderPort, 9000);
    // senderName is not deserialized in from_json — documents the gap
    EXPECT_EQ(result.senderName, "");
    EXPECT_TRUE(std::holds_alternative<PayloadEmpty>(result.payload));
}

// ---------------------------------------------------------------------------
// ConnectRequest
// ---------------------------------------------------------------------------

TEST(SignalPacketRoundtrip, ConnectRequest)
{
    SignalPacket p = makeBase(SignalType::ConnectRequest);
    p.payload      = PayloadEmpty{};
    auto result    = roundtrip(p);

    EXPECT_EQ(result.signalType, SignalType::ConnectRequest);
    EXPECT_TRUE(std::holds_alternative<PayloadEmpty>(result.payload));
}

// ---------------------------------------------------------------------------
// ConnectAnswer
// ---------------------------------------------------------------------------

TEST(SignalPacketRoundtrip, ConnectAnswerAccepted)
{
    SignalPacket p = makeBase(SignalType::ConnectAnswer);
    p.payload      = PayloadConnectAnswer{true};
    auto result    = roundtrip(p);

    ASSERT_TRUE(std::holds_alternative<PayloadConnectAnswer>(result.payload));
    EXPECT_TRUE(std::get<PayloadConnectAnswer>(result.payload).accepted);
}

TEST(SignalPacketRoundtrip, ConnectAnswerRejected)
{
    SignalPacket p = makeBase(SignalType::ConnectAnswer);
    p.payload      = PayloadConnectAnswer{false};
    auto result    = roundtrip(p);

    ASSERT_TRUE(std::holds_alternative<PayloadConnectAnswer>(result.payload));
    EXPECT_FALSE(std::get<PayloadConnectAnswer>(result.payload).accepted);
}

// ---------------------------------------------------------------------------
// Disconnect
// ---------------------------------------------------------------------------

TEST(SignalPacketRoundtrip, Disconnect)
{
    SignalPacket p = makeBase(SignalType::Disconnect);
    p.payload      = PayloadEmpty{};
    auto result    = roundtrip(p);

    EXPECT_EQ(result.signalType, SignalType::Disconnect);
    EXPECT_TRUE(std::holds_alternative<PayloadEmpty>(result.payload));
}

// ---------------------------------------------------------------------------
// DataPort
// ---------------------------------------------------------------------------

TEST(SignalPacketRoundtrip, DataPort)
{
    SignalPacket p = makeBase(SignalType::DataPort);
    p.payload      = PayloadDataPort{12345};
    auto result    = roundtrip(p);

    ASSERT_TRUE(std::holds_alternative<PayloadDataPort>(result.payload));
    EXPECT_EQ(std::get<PayloadDataPort>(result.payload).dataPort, 12345);
}

// ---------------------------------------------------------------------------
// ReadyFlag
// ---------------------------------------------------------------------------

TEST(SignalPacketRoundtrip, ReadyFlagTrue)
{
    SignalPacket p = makeBase(SignalType::ReadyFlag);
    p.payload      = PayloadReadyFlag{true};
    auto result    = roundtrip(p);

    ASSERT_TRUE(std::holds_alternative<PayloadReadyFlag>(result.payload));
    EXPECT_TRUE(std::get<PayloadReadyFlag>(result.payload).ready);
}

TEST(SignalPacketRoundtrip, ReadyFlagFalse)
{
    SignalPacket p = makeBase(SignalType::ReadyFlag);
    p.payload      = PayloadReadyFlag{false};
    auto result    = roundtrip(p);

    ASSERT_TRUE(std::holds_alternative<PayloadReadyFlag>(result.payload));
    EXPECT_FALSE(std::get<PayloadReadyFlag>(result.payload).ready);
}

// ---------------------------------------------------------------------------
// ValidationRequest
// ---------------------------------------------------------------------------

TEST(SignalPacketRoundtrip, ValidationRequest)
{
    SignalPacket p = makeBase(SignalType::ValidationRequest);
    p.payload      = PayloadValidationRequest{2}; // RemoteRequest::Version == 2
    auto result    = roundtrip(p);

    ASSERT_TRUE(std::holds_alternative<PayloadValidationRequest>(result.payload));
    EXPECT_EQ(std::get<PayloadValidationRequest>(result.payload).request, 2u);
}

// ---------------------------------------------------------------------------
// SecretResponse
// ---------------------------------------------------------------------------

TEST(SignalPacketRoundtrip, SecretResponse)
{
    SignalPacket p = makeBase(SignalType::SecretResponse);
    p.payload      = PayloadSecretResponse{"mysecret"};
    auto result    = roundtrip(p);

    ASSERT_TRUE(std::holds_alternative<PayloadSecretResponse>(result.payload));
    EXPECT_EQ(std::get<PayloadSecretResponse>(result.payload).secret, "mysecret");
}

// ---------------------------------------------------------------------------
// VersionResponse
// ---------------------------------------------------------------------------

TEST(SignalPacketRoundtrip, VersionResponse)
{
    SignalPacket p = makeBase(SignalType::VersionResponse);
    p.payload      = PayloadVersionResponse{"1.2.3"};
    auto result    = roundtrip(p);

    ASSERT_TRUE(std::holds_alternative<PayloadVersionResponse>(result.payload));
    EXPECT_EQ(std::get<PayloadVersionResponse>(result.payload).version, "1.2.3");
}

// ---------------------------------------------------------------------------
// ValidationHandshake (exercises default: branch in from_json)
// ---------------------------------------------------------------------------

TEST(SignalPacketRoundtrip, ValidationHandshake)
{
    SignalPacket p = makeBase(SignalType::ValidationHandshake);
    p.payload      = PayloadEmpty{};
    auto result    = roundtrip(p);

    EXPECT_EQ(result.signalType, SignalType::ValidationHandshake);
    EXPECT_TRUE(std::holds_alternative<PayloadEmpty>(result.payload));
}

// ---------------------------------------------------------------------------
// Numeric enum values — guard against accidental reordering
// ---------------------------------------------------------------------------

TEST(SignalPacketRoundtrip, SignalTypeNumericValues)
{
    {
        SignalPacket p = makeBase(SignalType::ConnectRequest);
        p.payload      = PayloadEmpty{};
        nlohmann::json j = p;
        EXPECT_EQ(j[JSON_Serialization::SignalType].get<int>(), 0);
    }
    {
        SignalPacket p = makeBase(SignalType::ValidationHandshake);
        p.payload      = PayloadEmpty{};
        nlohmann::json j = p;
        EXPECT_EQ(j[JSON_Serialization::SignalType].get<int>(), 8);
    }
}
