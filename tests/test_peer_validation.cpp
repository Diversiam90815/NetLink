#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "PeerValidation/PeerValidationService.h"

using namespace netlink;
using namespace std::chrono_literals;


namespace
{
DiscoveryEndpoint makeEndpoint(const std::string &name, const std::string &ip = "10.0.0.1", int port = 5000)
{
	return DiscoveryEndpoint{ip, port, name};
}

PeerValidationSendCallbacks makeNullCallbacks()
{
	return {}; // all function<> members are default-constructed (null)
}

template <typename Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout = 1s)
{
	auto deadline = std::chrono::steady_clock::now() + timeout;
	while (std::chrono::steady_clock::now() < deadline)
	{
		if (predicate())
			return true;
		std::this_thread::sleep_for(5ms);
	}
	return predicate();
}
} // namespace


// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

TEST(PeerValidationService, DefaultConfigDoesNotCrash)
{
	PeerValidationService svc;
	EXPECT_NO_THROW(svc.setConfig(PeerValidationConfig{})) << "Applying a default-constructed config must not throw";
}


// ---------------------------------------------------------------------------
// validatePeer — basic paths
// ---------------------------------------------------------------------------

TEST(PeerValidationService, ValidatePeer_ResultIncomplete_WhenBothChecksDisabled)
{
	PeerValidationService svc;
	PeerValidationConfig  cfg;
	cfg.enableVersionCheck = false;
	cfg.enableSecretCheck  = false;
	svc.setConfig(cfg);
	svc.setSendCallbacks(makeNullCallbacks());

	auto result = svc.validatePeer(makeEndpoint("pc-a"));
	EXPECT_EQ(result.status, ValidationResult::Status::ResultIncomplete) << "With both checks disabled no validation can complete synchronously — status must be ResultIncomplete";
	EXPECT_EQ(result.remoteEndpoint.displayName, "pc-a") << "The returned result must carry the remote endpoint display name that was passed in";
}

TEST(PeerValidationService, ValidatePeer_WithNullSendCallbacks_DoesNotCrash)
{
	PeerValidationService svc;
	PeerValidationConfig  cfg;
	cfg.enableVersionCheck = true;
	cfg.enableSecretCheck  = true;
	svc.setConfig(cfg);
	// No send callbacks set — the service must guard against null function objects
	EXPECT_NO_THROW(svc.validatePeer(makeEndpoint("pc-b", "10.0.0.2"))) << "validatePeer() must not crash when send callbacks are null, even with checks enabled";
}


// ---------------------------------------------------------------------------
// validatePeer — callback interception
// ---------------------------------------------------------------------------

TEST(PeerValidationService, ValidatePeer_VersionCheck_SendsVersionRequest)
{
	PeerValidationService svc;
	PeerValidationConfig  cfg;
	cfg.enableVersionCheck		= true;
	cfg.enableSecretCheck		= false;
	cfg.versionRequestTimeoutMs = 3000;
	svc.setConfig(cfg);

	std::atomic<bool>			requestSent{false};
	std::string					capturedName;
	RemoteRequest				capturedReq{};

	PeerValidationSendCallbacks cbs;
	cbs.sendRequest = [&](const std::string &name, RemoteRequest req)
	{
		capturedName = name;
		capturedReq	 = req;
		requestSent.store(true);
	};
	svc.setSendCallbacks(cbs);

	svc.validatePeer(makeEndpoint("pc-c", "10.0.0.3"));

	EXPECT_TRUE(requestSent.load()) << "Enabling the version check must trigger an outgoing RemoteRequest via the sendRequest callback";
	EXPECT_EQ(capturedName, "pc-c") << "The sendRequest callback must receive the correct computer name";
	EXPECT_EQ(capturedReq, RemoteRequest::Version) << "The sendRequest callback must be called with RemoteRequest::Version when only the version check is enabled";
}

TEST(PeerValidationService, ValidatePeer_SecretCheck_SendsSecretRequest)
{
	PeerValidationService svc;
	PeerValidationConfig  cfg;
	cfg.enableVersionCheck	   = false;
	cfg.enableSecretCheck	   = true;
	cfg.secretRequestTimeoutMs = 3000;
	svc.setConfig(cfg);

	std::atomic<bool>			requestSent{false};
	RemoteRequest				capturedReq{};

	PeerValidationSendCallbacks cbs;
	cbs.sendRequest = [&](const std::string &, RemoteRequest req)
	{
		capturedReq = req;
		requestSent.store(true);
	};
	svc.setSendCallbacks(cbs);

	svc.validatePeer(makeEndpoint("pc-d", "10.0.0.4"));

	EXPECT_TRUE(requestSent.load()) << "Enabling the secret check must trigger an outgoing RemoteRequest via the sendRequest callback";
	EXPECT_EQ(capturedReq, RemoteRequest::Secret) << "The sendRequest callback must be called with RemoteRequest::Secret when only the secret check is enabled";
}

TEST(PeerValidationService, ValidatePeer_BothChecks_SendsBothRequestsConcurrently)
{
	PeerValidationService svc;
	PeerValidationConfig  cfg;
	cfg.enableVersionCheck		= true;
	cfg.enableSecretCheck		= true;
	cfg.versionRequestTimeoutMs = 3000;
	cfg.secretRequestTimeoutMs	= 3000;
	svc.setConfig(cfg);

	std::mutex					mutex;
	std::vector<RemoteRequest>	received;

	PeerValidationSendCallbacks cbs;
	cbs.sendRequest = [&](const std::string &, RemoteRequest req)
	{
		std::lock_guard<std::mutex> lock(mutex);
		received.push_back(req);
	};
	svc.setSendCallbacks(cbs);

	svc.validatePeer(makeEndpoint("pc-e", "10.0.0.5"));

	ASSERT_EQ(received.size(), 2u) << "With both checks enabled, exactly two requests must be sent — one for version and one for secret";
	EXPECT_NE(std::find(received.begin(), received.end(), RemoteRequest::Version), received.end())
		<< "A RemoteRequest::Version must be among the sent requests when the version check is enabled";
	EXPECT_NE(std::find(received.begin(), received.end(), RemoteRequest::Secret), received.end())
		<< "A RemoteRequest::Secret must be among the sent requests when the secret check is enabled";
}

// ---------------------------------------------------------------------------
// getValidatedPeers
// ---------------------------------------------------------------------------

TEST(PeerValidationService, GetValidatedPeers_InitiallyEmpty)
{
	PeerValidationService svc;
	EXPECT_TRUE(svc.getValidatedPeers().empty()) << "A fresh PeerValidationService must have no validated peers";
}

// ---------------------------------------------------------------------------
// onPeerDiscovered — handshake callback
// ---------------------------------------------------------------------------

TEST(PeerValidationService, OnPeerDiscovered_SendsHandshake)
{
	PeerValidationService svc;
	svc.setConfig(PeerValidationConfig{});

	std::atomic<bool>			handshakeSent{false};
	std::string					capturedName;

	PeerValidationSendCallbacks cbs;
	cbs.sendHandshake = [&](const std::string &name)
	{
		capturedName = name;
		handshakeSent.store(true);
	};
	svc.setSendCallbacks(cbs);

	svc.onPeerDiscovered(makeEndpoint("pc-f", "10.0.0.6"));

	EXPECT_TRUE(handshakeSent.load()) << "Discovering a new peer must immediately trigger an outgoing handshake via the sendHandshake callback";
	EXPECT_EQ(capturedName, "pc-f") << "The sendHandshake callback must receive the correct computer name of the discovered peer";
}

TEST(PeerValidationService, OnPeerDiscovered_ThenOnHandshakeReceived_TriggersValidation)
{
	PeerValidationService svc;
	PeerValidationConfig  cfg;
	cfg.enableSecretCheck  = false;
	cfg.enableVersionCheck = false;
	svc.setConfig(cfg);
	svc.setSendCallbacks(makeNullCallbacks());

	// Discover the peer (marks "sent" once handshake is dispatched), then simulate the
	// remote's handshake arriving. Only once BOTH sides are done must validation start.
	svc.onPeerDiscovered(makeEndpoint("pc-g", "10.0.0.7"));
	svc.onHandshakeReceived("pc-g");

	// With both checks disabled, validation completes synchronously as soon as it starts.
	EXPECT_TRUE(waitUntil([&] { return svc.getValidationResult("pc-g").has_value(); }))
		<< "Once the handshake completes (sent && received) validation must be started for the peer";
}

TEST(PeerValidationService, OnHandshakeReceived_BeforeDiscovery_DoesNotStartValidationPrematurely)
{
	PeerValidationService svc;
	svc.setConfig(PeerValidationConfig{});
	svc.setSendCallbacks(makeNullCallbacks());

	// Remote reaches us first — only "received" is true, "sent" is not yet.
	svc.onHandshakeReceived("pc-h");

	std::this_thread::sleep_for(50ms);
	EXPECT_FALSE(svc.getValidationResult("pc-h").has_value()) << "A handshake that has only been received (not yet sent) must NOT trigger validation — "
																 "this guards against the premature-completion bug in the original implementation";
}


// ---------------------------------------------------------------------------
// clearValidatedPeer
// ---------------------------------------------------------------------------

TEST(PeerValidationService, ClearValidatedPeer_NonExistent_DoesNotCrash)
{
	PeerValidationService svc;
	EXPECT_NO_THROW(svc.clearValidatedPeer("nonexistent")) << "clearValidatedPeer() must not throw or crash when the given name is not in the validated peers map";
}

TEST(PeerValidationService, ClearValidatedPeer_AllowsRevalidation)
{
	PeerValidationService svc;
	svc.setConfig(PeerValidationConfig{}); // both checks disabled -> completes synchronously
	svc.setSendCallbacks(makeNullCallbacks());

	svc.validatePeer(makeEndpoint("pc-i"));
	ASSERT_TRUE(waitUntil([&] { return svc.getValidationResult("pc-i").has_value(); }));

	svc.clearValidatedPeer("pc-i");

	auto result = svc.validatePeer(makeEndpoint("pc-i"));
	EXPECT_NE(result.status, ValidationResult::Status::AlreadyValidated) << "After clearValidatedPeer(), re-validating the same peer must not short-circuit with AlreadyValidated";
}


// ---------------------------------------------------------------------------
// Validation callback
// ---------------------------------------------------------------------------

TEST(PeerValidationService, ValidationCallback_FiresImmediately_WhenBothChecksDisabled)
{
	// With no checks registered there is nothing to wait for a remote response on, so
	// validatePeer() must complete validation synchronously and fire the callback right away.
	PeerValidationService svc;
	PeerValidationConfig  cfg;
	cfg.enableVersionCheck = false;
	cfg.enableSecretCheck  = false;
	svc.setConfig(cfg);
	svc.setSendCallbacks(makeNullCallbacks());

	std::atomic<bool> callbackFired{false};
	ValidationResult  capturedResult;
	svc.setValidationCallback(
		[&](const ValidationResult &r)
		{
			capturedResult = r;
			callbackFired.store(true);
		});

	svc.validatePeer(makeEndpoint("pc-j", "10.0.0.7"));

	EXPECT_TRUE(waitUntil([&] { return callbackFired.load(); })) << "With zero checks registered, validatePeer() must complete synchronously and fire the callback";
	EXPECT_EQ(capturedResult.status, ValidationResult::Status::ReadyToConnect) << "With no checks to fail, a peer with all checks disabled must be considered ReadyToConnect";
}


// ---------------------------------------------------------------------------
// End-to-end success / failure flows via onCheckResponseReceived
// ---------------------------------------------------------------------------

TEST(PeerValidationService, FullFlow_MatchingSecretAndVersion_ResultsInReadyToConnect)
{
	PeerValidationService svc;
	PeerValidationConfig  cfg;
	cfg.enableSecretCheck  = true;
	cfg.enableVersionCheck = true;
	svc.setConfig(cfg);
	svc.setLocalSecret("shared-secret");
	svc.setLocalVersion("1.0.0");
	svc.setSendCallbacks(makeNullCallbacks());

	std::atomic<bool> callbackFired{false};
	ValidationResult  capturedResult;
	svc.setValidationCallback(
		[&](const ValidationResult &r)
		{
			capturedResult = r;
			callbackFired.store(true);
		});

	svc.validatePeer(makeEndpoint("pc-k", "10.0.0.10"));

	svc.onCheckResponseReceived("pc-k", RemoteRequest::Secret, "shared-secret");
	svc.onCheckResponseReceived("pc-k", RemoteRequest::Version, "1.0.0");

	ASSERT_TRUE(waitUntil([&] { return callbackFired.load(); })) << "The validation callback must fire once all checks have received their remote data";
	EXPECT_EQ(capturedResult.status, ValidationResult::Status::ReadyToConnect);
	EXPECT_TRUE(capturedResult.canConnect);

	auto validated = svc.getValidatedPeers();
	ASSERT_EQ(validated.size(), 1u) << "A successfully validated peer must appear in getValidatedPeers()";
	EXPECT_EQ(validated.front().remoteEndpoint.displayName, "pc-k");
}

TEST(PeerValidationService, FullFlow_MismatchedSecret_ResultsInSecretMissmatch)
{
	PeerValidationService svc;
	PeerValidationConfig  cfg;
	cfg.enableSecretCheck  = true;
	cfg.enableVersionCheck = false;
	svc.setConfig(cfg);
	svc.setLocalSecret("expected-secret");
	svc.setSendCallbacks(makeNullCallbacks());

	std::atomic<bool> callbackFired{false};
	ValidationResult  capturedResult;
	svc.setValidationCallback(
		[&](const ValidationResult &r)
		{
			capturedResult = r;
			callbackFired.store(true);
		});

	svc.validatePeer(makeEndpoint("pc-l", "10.0.0.11"));
	svc.onCheckResponseReceived("pc-l", RemoteRequest::Secret, "wrong-secret");

	ASSERT_TRUE(waitUntil([&] { return callbackFired.load(); }));
	EXPECT_EQ(capturedResult.status, ValidationResult::Status::SecretMissmatch);
	EXPECT_FALSE(capturedResult.canConnect);
	EXPECT_TRUE(svc.getValidatedPeers().empty()) << "A peer that failed validation must not appear among ready-to-connect peers";
}

TEST(PeerValidationService, FullFlow_MismatchedVersion_ResultsInVersionMissmatchAndReportsRemoteVersion)
{
	PeerValidationService svc;
	PeerValidationConfig  cfg;
	cfg.enableSecretCheck  = false;
	cfg.enableVersionCheck = true;
	svc.setConfig(cfg);
	svc.setLocalVersion("2.0.0");
	svc.setSendCallbacks(makeNullCallbacks());

	std::atomic<bool> callbackFired{false};
	ValidationResult  capturedResult;
	svc.setValidationCallback(
		[&](const ValidationResult &r)
		{
			capturedResult = r;
			callbackFired.store(true);
		});

	svc.validatePeer(makeEndpoint("pc-m", "10.0.0.12"));
	svc.onCheckResponseReceived("pc-m", RemoteRequest::Version, "1.0.0");

	ASSERT_TRUE(waitUntil([&] { return callbackFired.load(); }));
	EXPECT_EQ(capturedResult.status, ValidationResult::Status::VersionMissmatch);
	EXPECT_EQ(capturedResult.remoteVersion, "1.0.0") << "On version mismatch the result must still report the remote's reported version";
}

TEST(PeerValidationService, AlreadyValidatedPeer_ReturnsCachedResultWithoutSendingNewRequests)
{
	PeerValidationService svc;
	PeerValidationConfig  cfg;
	cfg.enableSecretCheck = true;
	svc.setConfig(cfg);
	svc.setLocalSecret("secret");

	std::atomic<int>			requestCount{0};
	PeerValidationSendCallbacks cbs;
	cbs.sendRequest = [&](const std::string &, RemoteRequest) { requestCount.fetch_add(1); };
	svc.setSendCallbacks(cbs);

	svc.validatePeer(makeEndpoint("pc-n", "10.0.0.13"));
	svc.onCheckResponseReceived("pc-n", RemoteRequest::Secret, "secret");

	ASSERT_TRUE(waitUntil([&] { return svc.getValidationResult("pc-n").has_value(); }));
	int	 countAfterFirstValidation = requestCount.load();

	auto secondResult			   = svc.validatePeer(makeEndpoint("pc-n", "10.0.0.13"));

	EXPECT_EQ(secondResult.status, ValidationResult::Status::AlreadyValidated) << "Re-validating an already validated peer must short-circuit with AlreadyValidated";
	EXPECT_EQ(requestCount.load(), countAfterFirstValidation) << "No new remote requests should be sent for an already-validated peer";
}


// ---------------------------------------------------------------------------
// Incoming request handling
// ---------------------------------------------------------------------------

TEST(PeerValidationService, OnRequestReceived_Secret_SendsOurSecretBack)
{
	PeerValidationService svc;
	svc.setLocalSecret("my-secret");

	std::atomic<bool>			responseSent{false};
	std::string					sentValue;

	PeerValidationSendCallbacks cbs;
	cbs.sendSecretResponse = [&](const std::string &, const std::string &val)
	{
		sentValue = val;
		responseSent.store(true);
	};
	svc.setSendCallbacks(cbs);

	svc.onRequestReceived("remote-pc", RemoteRequest::Secret);

	EXPECT_TRUE(responseSent.load()) << "Receiving a Secret request must trigger sendSecretResponse";
	EXPECT_EQ(sentValue, "my-secret") << "Our local secret must be sent back in response to a Secret request";
}

TEST(PeerValidationService, OnRequestReceived_Version_SendsOurVersionBack)
{
	PeerValidationService svc;
	svc.setLocalVersion("3.2.1");

	std::atomic<bool>			responseSent{false};
	std::string					sentValue;

	PeerValidationSendCallbacks cbs;
	cbs.sendVersionResponse = [&](const std::string &, const std::string &val)
	{
		sentValue = val;
		responseSent.store(true);
	};
	svc.setSendCallbacks(cbs);

	svc.onRequestReceived("remote-pc", RemoteRequest::Version);

	EXPECT_TRUE(responseSent.load()) << "Receiving a Version request must trigger sendVersionResponse";
	EXPECT_EQ(sentValue, "3.2.1");
}


// ---------------------------------------------------------------------------
// getLastResult / getValidationResult
// ---------------------------------------------------------------------------

TEST(PeerValidationService, GetValidationResult_ReturnsNullopt_BeforeCompletion)
{
	PeerValidationService svc;
	EXPECT_FALSE(svc.getValidationResult("never-validated").has_value());
}

TEST(PeerValidationService, GetLastResult_ReflectsMostRecentValidatePeerCall)
{
	PeerValidationService svc;
	svc.setConfig(PeerValidationConfig{});
	svc.setSendCallbacks(makeNullCallbacks());

	svc.validatePeer(makeEndpoint("pc-o", "10.0.0.14"));

	EXPECT_EQ(svc.getLastResult().remoteEndpoint.displayName, "pc-o") << "getLastResult() must reflect the most recent validatePeer() invocation";
}


// ---------------------------------------------------------------------------
// Timeout-driven validation callbacks
// ---------------------------------------------------------------------------

TEST(PeerValidationService, VersionTimeout_Fires_WithTimedoutStatus)
{
	PeerValidationService svc;
	PeerValidationConfig  cfg;
	cfg.enableVersionCheck		= true;
	cfg.enableSecretCheck		= false;
	cfg.versionRequestTimeoutMs = 150;
	svc.setConfig(cfg);

	PeerValidationSendCallbacks cbs;
	cbs.sendRequest = [](const std::string &, RemoteRequest) {};
	svc.setSendCallbacks(cbs);

	std::atomic<bool> callbackFired{false};
	ValidationResult  capturedResult;
	svc.setValidationCallback(
		[&](const ValidationResult &r)
		{
			capturedResult = r;
			callbackFired.store(true);
		});

	svc.validatePeer(makeEndpoint("pc-h", "10.0.0.8"));

	EXPECT_TRUE(waitUntil([&] { return callbackFired.load(); }, 800ms)) << "The validation callback must fire after the version request timeout (150 ms) elapses with no response";
	EXPECT_EQ(capturedResult.status, ValidationResult::Status::ValidationTimedout) << "When the version request times out the callback must report ValidationTimedout status";
}

TEST(PeerValidationService, SecretTimeout_Fires_WithTimedoutStatus)
{
	PeerValidationService svc;
	PeerValidationConfig  cfg;
	cfg.enableVersionCheck	   = false;
	cfg.enableSecretCheck	   = true;
	cfg.secretRequestTimeoutMs = 150;
	svc.setConfig(cfg);

	PeerValidationSendCallbacks cbs;
	cbs.sendRequest = [](const std::string &, RemoteRequest) {};
	svc.setSendCallbacks(cbs);

	std::atomic<bool> callbackFired{false};
	ValidationResult  capturedResult;
	svc.setValidationCallback(
		[&](const ValidationResult &r)
		{
			capturedResult = r;
			callbackFired.store(true);
		});

	svc.validatePeer(makeEndpoint("pc-i", "10.0.0.9"));

	EXPECT_TRUE(waitUntil([&] { return callbackFired.load(); }, 800ms)) << "The validation callback must fire after the secret request timeout (150 ms) elapses with no response";
	EXPECT_EQ(capturedResult.status, ValidationResult::Status::ValidationTimedout) << "When the secret request times out the callback must report ValidationTimedout status";
}

TEST(PeerValidationService, OneCheckTimesOutBeforeOtherResponds_StillReportsTimeout)
{
	// With both checks enabled, if only ONE of them times out, the overall validation
	// must still fail with ValidationTimedout even though the other check received data.
	PeerValidationService svc;
	PeerValidationConfig  cfg;
	cfg.enableSecretCheck		= true;
	cfg.enableVersionCheck		= true;
	cfg.secretRequestTimeoutMs	= 150;
	cfg.versionRequestTimeoutMs = 5000; // effectively won't fire during this test
	svc.setConfig(cfg);
	svc.setLocalVersion("1.0.0");

	PeerValidationSendCallbacks cbs;
	cbs.sendRequest = [](const std::string &, RemoteRequest) {};
	svc.setSendCallbacks(cbs);

	std::atomic<bool> callbackFired{false};
	ValidationResult  capturedResult;
	svc.setValidationCallback(
		[&](const ValidationResult &r)
		{
			capturedResult = r;
			callbackFired.store(true);
		});

	svc.validatePeer(makeEndpoint("pc-p", "10.0.0.15"));
	svc.onCheckResponseReceived("pc-p", RemoteRequest::Version, "1.0.0"); // version arrives fine

	EXPECT_TRUE(waitUntil([&] { return callbackFired.load(); }, 800ms)) << "The secret check timing out must still fail the overall validation even if version succeeded";
	EXPECT_EQ(capturedResult.status, ValidationResult::Status::ValidationTimedout);
}


TEST(PeerValidationService, ValidatePeer_NoChecksRegistered_CompletesAndIsRetrievable)
{
	PeerValidationService svc;
	svc.setConfig(PeerValidationConfig{}); // both checks disabled
	svc.setSendCallbacks(makeNullCallbacks());

	svc.validatePeer(makeEndpoint("pc-q", "10.0.0.20"));

	ASSERT_TRUE(waitUntil([&] { return svc.getValidationResult("pc-q").has_value(); }, 200ms)) << "validatePeer() must complete immediately when there are no checks to wait on";

	auto ready = svc.getValidatedPeers();
	ASSERT_EQ(ready.size(), 1u);
	EXPECT_EQ(ready.front().remoteEndpoint.displayName, "pc-q");
}