#include <gtest/gtest.h>
#include <thread>
#include <vector>

#include "PeerValidation/Checks/SecretCompatibilityCheck.h"
#include "PeerValidation/Checks/VersionCompatibilityCheck.h"

using namespace netlink;


// ---------------------------------------------------------------------------
// SecretCompatibilityCheck
// ---------------------------------------------------------------------------

TEST(SecretCompatibilityCheck, NameAndWireType_AreStable)
{
	SecretCompatibilityCheck check("secret123");
	EXPECT_EQ(check.name(), "secret_request");
	EXPECT_TRUE(check.requiresRemoteRequest());
	ASSERT_TRUE(check.wireRequestType().has_value());
	EXPECT_EQ(*check.wireRequestType(), RemoteRequest::Secret);
}


TEST(SecretCompatibilityCheck, IsReady_FalseBeforeDataReceived)
{
	SecretCompatibilityCheck check("secret123");
	EXPECT_FALSE(check.isReady("pc-a"));
}


TEST(SecretCompatibilityCheck, IsReady_TrueAfterDataReceived)
{
	SecretCompatibilityCheck check("secret123");
	check.onRemoteDataReceived("pc-a", "secret123");
	EXPECT_TRUE(check.isReady("pc-a"));
}


TEST(SecretCompatibilityCheck, Evaluate_MatchingSecret_ReturnsTrue)
{
	SecretCompatibilityCheck check("secret123");
	check.onRemoteDataReceived("pc-a", "secret123");
	EXPECT_TRUE(check.evaluate("pc-a"));
}


TEST(SecretCompatibilityCheck, Evaluate_MismatchedSecret_ReturnsFalse)
{
	SecretCompatibilityCheck check("secret123");
	check.onRemoteDataReceived("pc-a", "different-secret");
	EXPECT_FALSE(check.evaluate("pc-a"));
}


TEST(SecretCompatibilityCheck, Evaluate_NoDataReceived_ReturnsFalse)
{
	SecretCompatibilityCheck check("secret123");
	EXPECT_FALSE(check.evaluate("unknown-peer")) << "evaluate() must fail safe (false) when no data was ever received";
}


TEST(SecretCompatibilityCheck, FailureMessage_IsNonEmpty)
{
	SecretCompatibilityCheck check("secret123");
	EXPECT_FALSE(check.failureMessage("pc-a").empty());
}


TEST(SecretCompatibilityCheck, Reset_ClearsPerPeerState)
{
	SecretCompatibilityCheck check("secret123");
	check.onRemoteDataReceived("pc-a", "secret123");
	ASSERT_TRUE(check.isReady("pc-a"));

	check.reset("pc-a");

	EXPECT_FALSE(check.isReady("pc-a")) << "reset() must clear received data for that peer";
}


TEST(SecretCompatibilityCheck, SetLocalSecret_AffectsFutureEvaluations)
{
	SecretCompatibilityCheck check("old-secret");
	check.onRemoteDataReceived("pc-a", "new-secret");
	ASSERT_FALSE(check.evaluate("pc-a"));

	check.setLocalSecret("new-secret");

	EXPECT_TRUE(check.evaluate("pc-a")) << "Updating the local secret must affect subsequent evaluate() calls";
}


TEST(SecretCompatibilityCheck, MultiplePeers_AreIndependentlyTracked)
{
	SecretCompatibilityCheck check("shared-secret");
	check.onRemoteDataReceived("pc-a", "shared-secret");
	check.onRemoteDataReceived("pc-b", "wrong-secret");

	EXPECT_TRUE(check.evaluate("pc-a"));
	EXPECT_FALSE(check.evaluate("pc-b"));
	EXPECT_FALSE(check.isReady("pc-c")) << "A peer that never sent data must remain not-ready, independent of other peers";
}


TEST(SecretCompatibilityCheck, ConcurrentAccess_DifferentPeers_IsThreadSafe)
{
	SecretCompatibilityCheck check("shared-secret");
	constexpr int			 peerCount = 30;
	std::vector<std::thread> threads;

	for (int i = 0; i < peerCount; ++i)
	{
		threads.emplace_back([&check, i] { check.onRemoteDataReceived("pc-" + std::to_string(i), "shared-secret"); });
	}

	for (auto &t : threads)
		t.join();

	for (int i = 0; i < peerCount; ++i)
		EXPECT_TRUE(check.evaluate("pc-" + std::to_string(i)));
}


// ---------------------------------------------------------------------------
// VersionCompatibilityCheck
// ---------------------------------------------------------------------------

TEST(VersionCompatibilityCheck, NameAndWireType_AreStable)
{
	VersionCompatibilityCheck check("1.2.3");
	EXPECT_EQ(check.name(), "version_request");
	EXPECT_TRUE(check.requiresRemoteRequest());
	ASSERT_TRUE(check.wireRequestType().has_value());
	EXPECT_EQ(*check.wireRequestType(), RemoteRequest::Version);
}


TEST(VersionCompatibilityCheck, Evaluate_MatchingVersion_ReturnsTrue)
{
	VersionCompatibilityCheck check("1.2.3");
	check.onRemoteDataReceived("pc-a", "1.2.3");
	EXPECT_TRUE(check.evaluate("pc-a"));
}


TEST(VersionCompatibilityCheck, Evaluate_MismatchedVersion_ReturnsFalse)
{
	VersionCompatibilityCheck check("1.2.3");
	check.onRemoteDataReceived("pc-a", "1.9.9");
	EXPECT_FALSE(check.evaluate("pc-a"));
}


TEST(VersionCompatibilityCheck, RemoteVersion_ReturnsReceivedValue)
{
	VersionCompatibilityCheck check("1.2.3");
	check.onRemoteDataReceived("pc-a", "9.9.9");
	EXPECT_EQ(check.remoteVersion("pc-a"), "9.9.9");
}


TEST(VersionCompatibilityCheck, RemoteVersion_EmptyWhenNoDataReceived)
{
	VersionCompatibilityCheck check("1.2.3");
	EXPECT_TRUE(check.remoteVersion("unknown").empty());
}


TEST(VersionCompatibilityCheck, Reset_ClearsPerPeerState)
{
	VersionCompatibilityCheck check("1.2.3");
	check.onRemoteDataReceived("pc-a", "1.2.3");

	check.reset("pc-a");

	EXPECT_FALSE(check.isReady("pc-a"));
	EXPECT_TRUE(check.remoteVersion("pc-a").empty());
}


TEST(VersionCompatibilityCheck, SetLocalVersion_AffectsFutureEvaluations)
{
	VersionCompatibilityCheck check("1.0.0");
	check.onRemoteDataReceived("pc-a", "2.0.0");
	ASSERT_FALSE(check.evaluate("pc-a"));

	check.setLocalVersion("2.0.0");

	EXPECT_TRUE(check.evaluate("pc-a"));
}
