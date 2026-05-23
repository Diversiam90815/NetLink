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
    EXPECT_FALSE(p.isComplete());
}

TEST(PendingValidation, IsCompleteVersionOnly)
{
    PendingValidation p;
    p.versionReceived = true;
    EXPECT_FALSE(p.isComplete());
}

TEST(PendingValidation, IsCompleteSecretOnly)
{
    PendingValidation p;
    p.secretReceived = true;
    EXPECT_FALSE(p.isComplete());
}

TEST(PendingValidation, IsCompleteWhenBothSet)
{
    PendingValidation p;
    p.versionReceived = true;
    p.secretReceived  = true;
    EXPECT_TRUE(p.isComplete());
}

// ---------------------------------------------------------------------------
// ValidationResult::isReadyToConnect
// ---------------------------------------------------------------------------

TEST(ValidationResult, IsReadyToConnect)
{
    ValidationResult r;
    r.status = ValidationResult::Status::ReadyToConnect;
    EXPECT_TRUE(r.isReadyToConnect());
}

TEST(ValidationResult, IsNotReadyForVersionMismatch)
{
    ValidationResult r;
    r.status = ValidationResult::Status::VersionMissmatch;
    EXPECT_FALSE(r.isReadyToConnect());
}

TEST(ValidationResult, IsNotReadyForSecretMismatch)
{
    ValidationResult r;
    r.status = ValidationResult::Status::SecretMissmatch;
    EXPECT_FALSE(r.isReadyToConnect());
}

TEST(ValidationResult, IsNotReadyForPeerInvalid)
{
    ValidationResult r;
    r.status = ValidationResult::Status::PeerInvalid;
    EXPECT_FALSE(r.isReadyToConnect());
}

TEST(ValidationResult, IsNotReadyForAlreadyValidated)
{
    ValidationResult r;
    r.status = ValidationResult::Status::AlreadyValidated;
    EXPECT_FALSE(r.isReadyToConnect());
}

TEST(ValidationResult, IsNotReadyForResultIncomplete)
{
    ValidationResult r;
    r.status = ValidationResult::Status::ResultIncomplete;
    EXPECT_FALSE(r.isReadyToConnect());
}

TEST(ValidationResult, IsNotReadyForRemoteRemoved)
{
    ValidationResult r;
    r.status = ValidationResult::Status::RemoteRemoved;
    EXPECT_FALSE(r.isReadyToConnect());
}

TEST(ValidationResult, IsNotReadyForValidationTimedout)
{
    ValidationResult r;
    r.status = ValidationResult::Status::ValidationTimedout;
    EXPECT_FALSE(r.isReadyToConnect());
}

TEST(ValidationResult, StatusEnumValues)
{
    // Guard against accidental enum reordering
    EXPECT_EQ(static_cast<int>(ValidationResult::Status::ReadyToConnect),   1);
    EXPECT_EQ(static_cast<int>(ValidationResult::Status::VersionMissmatch), 2);
    EXPECT_EQ(static_cast<int>(ValidationResult::Status::SecretMissmatch),  3);
    EXPECT_EQ(static_cast<int>(ValidationResult::Status::PeerInvalid),      4);
    EXPECT_EQ(static_cast<int>(ValidationResult::Status::AlreadyValidated), 5);
    EXPECT_EQ(static_cast<int>(ValidationResult::Status::ResultIncomplete), 6);
    EXPECT_EQ(static_cast<int>(ValidationResult::Status::RemoteRemoved),    7);
    EXPECT_EQ(static_cast<int>(ValidationResult::Status::ValidationTimedout), 8);
}

// ---------------------------------------------------------------------------
// RemoteHandshake::isComplete
// ---------------------------------------------------------------------------

TEST(RemoteHandshake, IsCompleteDefaultFalse)
{
    RemoteHandshake h;
    EXPECT_FALSE(h.isComplete());
}

TEST(RemoteHandshake, IsCompleteSentOnly)
{
    RemoteHandshake h;
    h.sent = true;
    EXPECT_FALSE(h.isComplete());
}

TEST(RemoteHandshake, IsCompleteReceivedOnly)
{
    RemoteHandshake h;
    h.received = true;
    EXPECT_FALSE(h.isComplete());
}

TEST(RemoteHandshake, IsCompleteWhenBothTrue)
{
    RemoteHandshake h;
    h.sent     = true;
    h.received = true;
    EXPECT_TRUE(h.isComplete());
}

// ---------------------------------------------------------------------------
// ConnectionStatusUpdate::getTypeString
// ---------------------------------------------------------------------------

TEST(ConnectionStatusUpdate, GetTypeStringInitiated)
{
    ConnectionStatusUpdate u;
    u.type = ConnectionStatusUpdate::Type::Initiated;
    EXPECT_EQ(u.getTypeString(), "Initiated");
}

TEST(ConnectionStatusUpdate, GetTypeStringInvitationSent)
{
    ConnectionStatusUpdate u;
    u.type = ConnectionStatusUpdate::Type::InvitationSent;
    EXPECT_EQ(u.getTypeString(), "Invitation Sent");
}

TEST(ConnectionStatusUpdate, GetTypeStringInvitationReceived)
{
    ConnectionStatusUpdate u;
    u.type = ConnectionStatusUpdate::Type::InvitationReceived;
    EXPECT_EQ(u.getTypeString(), "Invitation Received");
}

TEST(ConnectionStatusUpdate, GetTypeStringAccepted)
{
    ConnectionStatusUpdate u;
    u.type = ConnectionStatusUpdate::Type::Accepted;
    EXPECT_EQ(u.getTypeString(), "Accepted");
}

TEST(ConnectionStatusUpdate, GetTypeStringDeclined)
{
    ConnectionStatusUpdate u;
    u.type = ConnectionStatusUpdate::Type::Declined;
    EXPECT_EQ(u.getTypeString(), "Declined");
}

TEST(ConnectionStatusUpdate, GetTypeStringEstablishing)
{
    ConnectionStatusUpdate u;
    u.type = ConnectionStatusUpdate::Type::Establishing;
    EXPECT_EQ(u.getTypeString(), "Establishing");
}

TEST(ConnectionStatusUpdate, GetTypeStringEstablished)
{
    ConnectionStatusUpdate u;
    u.type = ConnectionStatusUpdate::Type::Established;
    EXPECT_EQ(u.getTypeString(), "Established");
}

TEST(ConnectionStatusUpdate, GetTypeStringFailed)
{
    ConnectionStatusUpdate u;
    u.type = ConnectionStatusUpdate::Type::Failed;
    EXPECT_EQ(u.getTypeString(), "Failed");
}

TEST(ConnectionStatusUpdate, GetTypeStringClosing)
{
    ConnectionStatusUpdate u;
    u.type = ConnectionStatusUpdate::Type::Closing;
    EXPECT_EQ(u.getTypeString(), "Closing");
}

TEST(ConnectionStatusUpdate, GetTypeStringClosed)
{
    ConnectionStatusUpdate u;
    u.type = ConnectionStatusUpdate::Type::Closed;
    EXPECT_EQ(u.getTypeString(), "Closed");
}
