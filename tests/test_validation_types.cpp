#include <gtest/gtest.h>
#include "PeerValidation/ValidationResult.h"
#include "PeerValidation/PeerValidationService.h"
#include "ConnectionService/ConnectionPhase.h"

using namespace netlink;

// ---------------------------------------------------------------------------
// PendingValidation::isComplete
// ---------------------------------------------------------------------------

TEST(PendingValidation, IsCompleteDefaultFalse)
{
    PendingValidation p;
    EXPECT_FALSE(p.isComplete())
        << "A default-constructed PendingValidation must not be complete — neither response has been received";
}

TEST(PendingValidation, IsCompleteVersionOnly)
{
    PendingValidation p;
    p.versionReceived = true;
    EXPECT_FALSE(p.isComplete())
        << "Receiving only the version response must not complete validation — the secret response is still missing";
}

TEST(PendingValidation, IsCompleteSecretOnly)
{
    PendingValidation p;
    p.secretReceived = true;
    EXPECT_FALSE(p.isComplete())
        << "Receiving only the secret response must not complete validation — the version response is still missing";
}

TEST(PendingValidation, IsCompleteWhenBothSet)
{
    PendingValidation p;
    p.versionReceived = true;
    p.secretReceived  = true;
    EXPECT_TRUE(p.isComplete())
        << "Validation must be complete only when both version and secret responses have been received";
}

// ---------------------------------------------------------------------------
// ValidationResult::isReadyToConnect
// ---------------------------------------------------------------------------

TEST(ValidationResult, IsReadyToConnect)
{
    ValidationResult r;
    r.status = ValidationResult::Status::ReadyToConnect;
    EXPECT_TRUE(r.isReadyToConnect())
        << "isReadyToConnect() must return true exactly when status is ReadyToConnect";
}

TEST(ValidationResult, IsNotReadyForVersionMismatch)
{
    ValidationResult r;
    r.status = ValidationResult::Status::VersionMissmatch;
    EXPECT_FALSE(r.isReadyToConnect())
        << "A version mismatch means the peer is incompatible — must not be ready to connect";
}

TEST(ValidationResult, IsNotReadyForSecretMismatch)
{
    ValidationResult r;
    r.status = ValidationResult::Status::SecretMissmatch;
    EXPECT_FALSE(r.isReadyToConnect())
        << "A secret mismatch means the peer is not trusted — must not be ready to connect";
}

TEST(ValidationResult, IsNotReadyForPeerInvalid)
{
    ValidationResult r;
    r.status = ValidationResult::Status::PeerInvalid;
    EXPECT_FALSE(r.isReadyToConnect())
        << "An invalid peer must not be ready to connect";
}

TEST(ValidationResult, IsNotReadyForAlreadyValidated)
{
    ValidationResult r;
    r.status = ValidationResult::Status::AlreadyValidated;
    EXPECT_FALSE(r.isReadyToConnect())
        << "AlreadyValidated is an informational status — it must not be treated as ready-to-connect on its own";
}

TEST(ValidationResult, IsNotReadyForResultIncomplete)
{
    ValidationResult r;
    r.status = ValidationResult::Status::ResultIncomplete;
    EXPECT_FALSE(r.isReadyToConnect())
        << "An incomplete validation result means checks are still pending — must not be ready to connect";
}

TEST(ValidationResult, IsNotReadyForRemoteRemoved)
{
    ValidationResult r;
    r.status = ValidationResult::Status::RemoteRemoved;
    EXPECT_FALSE(r.isReadyToConnect())
        << "A remote that has been removed must not be ready to connect";
}

TEST(ValidationResult, IsNotReadyForValidationTimedout)
{
    ValidationResult r;
    r.status = ValidationResult::Status::ValidationTimedout;
    EXPECT_FALSE(r.isReadyToConnect())
        << "A timed-out validation must not be ready to connect";
}

TEST(ValidationResult, StatusEnumValues)
{
    // Guard against accidental enum reordering — the numeric values are part of the protocol
    EXPECT_EQ(static_cast<int>(ValidationResult::Status::ReadyToConnect),     1)
        << "ReadyToConnect must have numeric value 1";
    EXPECT_EQ(static_cast<int>(ValidationResult::Status::VersionMissmatch),   2)
        << "VersionMissmatch must have numeric value 2";
    EXPECT_EQ(static_cast<int>(ValidationResult::Status::SecretMissmatch),    3)
        << "SecretMissmatch must have numeric value 3";
    EXPECT_EQ(static_cast<int>(ValidationResult::Status::PeerInvalid),        4)
        << "PeerInvalid must have numeric value 4";
    EXPECT_EQ(static_cast<int>(ValidationResult::Status::AlreadyValidated),   5)
        << "AlreadyValidated must have numeric value 5";
    EXPECT_EQ(static_cast<int>(ValidationResult::Status::ResultIncomplete),   6)
        << "ResultIncomplete must have numeric value 6";
    EXPECT_EQ(static_cast<int>(ValidationResult::Status::RemoteRemoved),      7)
        << "RemoteRemoved must have numeric value 7";
    EXPECT_EQ(static_cast<int>(ValidationResult::Status::ValidationTimedout), 8)
        << "ValidationTimedout must have numeric value 8";
}

// ---------------------------------------------------------------------------
// RemoteHandshake::isComplete
// ---------------------------------------------------------------------------

TEST(RemoteHandshake, IsCompleteDefaultFalse)
{
    RemoteHandshake h;
    EXPECT_FALSE(h.isComplete())
        << "A default-constructed RemoteHandshake must not be complete — neither side has exchanged anything yet";
}

TEST(RemoteHandshake, IsCompleteSentOnly)
{
    RemoteHandshake h;
    h.sent = true;
    EXPECT_FALSE(h.isComplete())
        << "Sending a handshake but not yet receiving one must not be considered complete";
}

TEST(RemoteHandshake, IsCompleteReceivedOnly)
{
    RemoteHandshake h;
    h.received = true;
    EXPECT_FALSE(h.isComplete())
        << "Receiving a handshake but not yet sending one must not be considered complete";
}

TEST(RemoteHandshake, IsCompleteWhenBothTrue)
{
    RemoteHandshake h;
    h.sent     = true;
    h.received = true;
    EXPECT_TRUE(h.isComplete())
        << "A handshake is only complete when both the outgoing and incoming sides have been exchanged";
}

// ---------------------------------------------------------------------------
// ConnectionStatusUpdate::getTypeString
// ---------------------------------------------------------------------------

TEST(ConnectionStatusUpdate, GetTypeStringInitiated)
{
    ConnectionStatusUpdate u;
    u.type = ConnectionStatusUpdate::Type::Initiated;
    EXPECT_EQ(u.getTypeString(), "Initiated")
        << "Type::Initiated must produce the string 'Initiated'";
}

TEST(ConnectionStatusUpdate, GetTypeStringInvitationSent)
{
    ConnectionStatusUpdate u;
    u.type = ConnectionStatusUpdate::Type::InvitationSent;
    EXPECT_EQ(u.getTypeString(), "Invitation Sent")
        << "Type::InvitationSent must produce the string 'Invitation Sent'";
}

TEST(ConnectionStatusUpdate, GetTypeStringInvitationReceived)
{
    ConnectionStatusUpdate u;
    u.type = ConnectionStatusUpdate::Type::InvitationReceived;
    EXPECT_EQ(u.getTypeString(), "Invitation Received")
        << "Type::InvitationReceived must produce 'Invitation Received' — this case must be handled in the switch";
}

TEST(ConnectionStatusUpdate, GetTypeStringAccepted)
{
    ConnectionStatusUpdate u;
    u.type = ConnectionStatusUpdate::Type::Accepted;
    EXPECT_EQ(u.getTypeString(), "Accepted")
        << "Type::Accepted must produce the string 'Accepted'";
}

TEST(ConnectionStatusUpdate, GetTypeStringDeclined)
{
    ConnectionStatusUpdate u;
    u.type = ConnectionStatusUpdate::Type::Declined;
    EXPECT_EQ(u.getTypeString(), "Declined")
        << "Type::Declined must produce the string 'Declined'";
}

TEST(ConnectionStatusUpdate, GetTypeStringEstablishing)
{
    ConnectionStatusUpdate u;
    u.type = ConnectionStatusUpdate::Type::Establishing;
    EXPECT_EQ(u.getTypeString(), "Establishing")
        << "Type::Establishing must produce the string 'Establishing'";
}

TEST(ConnectionStatusUpdate, GetTypeStringEstablished)
{
    ConnectionStatusUpdate u;
    u.type = ConnectionStatusUpdate::Type::Established;
    EXPECT_EQ(u.getTypeString(), "Established")
        << "Type::Established must produce the string 'Established'";
}

TEST(ConnectionStatusUpdate, GetTypeStringFailed)
{
    ConnectionStatusUpdate u;
    u.type = ConnectionStatusUpdate::Type::Failed;
    EXPECT_EQ(u.getTypeString(), "Failed")
        << "Type::Failed must produce the string 'Failed'";
}

TEST(ConnectionStatusUpdate, GetTypeStringClosing)
{
    ConnectionStatusUpdate u;
    u.type = ConnectionStatusUpdate::Type::Closing;
    EXPECT_EQ(u.getTypeString(), "Closing")
        << "Type::Closing must produce the string 'Closing'";
}

TEST(ConnectionStatusUpdate, GetTypeStringClosed)
{
    ConnectionStatusUpdate u;
    u.type = ConnectionStatusUpdate::Type::Closed;
    EXPECT_EQ(u.getTypeString(), "Closed")
        << "Type::Closed must produce the string 'Closed'";
}
