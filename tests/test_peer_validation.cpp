#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include "PeerValidation/PeerValidationService.h"

using namespace netlink;
using namespace std::chrono_literals;

namespace
{
DiscoveryEndpoint makeEndpoint(const std::string &name,
                                const std::string &ip   = "10.0.0.1",
                                int                port = 5000)
{
    return DiscoveryEndpoint{ip, port, name};
}

PeerValidationSendCallbacks makeNullCallbacks()
{
    return {}; // all function<> members are default-constructed (null)
}
} // namespace

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

TEST(PeerValidationService, DefaultConfigDoesNotCrash)
{
    PeerValidationService svc;
    EXPECT_NO_THROW(svc.setConfig(PeerValidationConfig{}))
        << "Applying a default-constructed config must not throw";
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
    EXPECT_EQ(result.status, ValidationResult::Status::ResultIncomplete)
        << "With both checks disabled no validation can complete synchronously — status must be ResultIncomplete";
    EXPECT_EQ(result.remoteEndpoint.displayName, "pc-a")
        << "The returned result must carry the remote endpoint display name that was passed in";
}

TEST(PeerValidationService, ValidatePeer_WithNullSendCallbacks_DoesNotCrash)
{
    PeerValidationService svc;
    PeerValidationConfig  cfg;
    cfg.enableVersionCheck = true;
    cfg.enableSecretCheck  = true;
    svc.setConfig(cfg);
    // No send callbacks set — the service must guard against null function objects
    EXPECT_NO_THROW(svc.validatePeer(makeEndpoint("pc-b", "10.0.0.2")))
        << "validatePeer() must not crash when send callbacks are null, even with checks enabled";
}

// ---------------------------------------------------------------------------
// validatePeer — callback interception
// ---------------------------------------------------------------------------

TEST(PeerValidationService, ValidatePeer_VersionCheck_SendsVersionRequest)
{
    PeerValidationService svc;
    PeerValidationConfig  cfg;
    cfg.enableVersionCheck      = true;
    cfg.enableSecretCheck       = false;
    cfg.versionRequestTimeoutMs = 3000;
    svc.setConfig(cfg);

    std::atomic<bool> requestSent{false};
    std::string       capturedName;
    RemoteRequest     capturedReq{};

    PeerValidationSendCallbacks cbs;
    cbs.sendRequest = [&](const std::string &name, RemoteRequest req)
    {
        capturedName = name;
        capturedReq  = req;
        requestSent.store(true);
    };
    svc.setSendCallbacks(cbs);

    svc.validatePeer(makeEndpoint("pc-c", "10.0.0.3"));

    EXPECT_TRUE(requestSent.load())
        << "Enabling the version check must trigger an outgoing RemoteRequest via the sendRequest callback";
    EXPECT_EQ(capturedName, "pc-c")
        << "The sendRequest callback must receive the correct computer name";
    EXPECT_EQ(capturedReq, RemoteRequest::Version)
        << "The sendRequest callback must be called with RemoteRequest::Version when only the version check is enabled";
}

TEST(PeerValidationService, ValidatePeer_SecretCheck_SendsSecretRequest)
{
    PeerValidationService svc;
    PeerValidationConfig  cfg;
    cfg.enableVersionCheck     = false;
    cfg.enableSecretCheck      = true;
    cfg.secretRequestTimeoutMs = 3000;
    svc.setConfig(cfg);

    std::atomic<bool> requestSent{false};
    RemoteRequest     capturedReq{};

    PeerValidationSendCallbacks cbs;
    cbs.sendRequest = [&](const std::string &, RemoteRequest req)
    {
        capturedReq = req;
        requestSent.store(true);
    };
    svc.setSendCallbacks(cbs);

    svc.validatePeer(makeEndpoint("pc-d", "10.0.0.4"));

    EXPECT_TRUE(requestSent.load())
        << "Enabling the secret check must trigger an outgoing RemoteRequest via the sendRequest callback";
    EXPECT_EQ(capturedReq, RemoteRequest::Secret)
        << "The sendRequest callback must be called with RemoteRequest::Secret when only the secret check is enabled";
}

TEST(PeerValidationService, ValidatePeer_BothChecks_SendsBothRequests)
{
    PeerValidationService svc;
    PeerValidationConfig  cfg;
    cfg.enableVersionCheck      = true;
    cfg.enableSecretCheck       = true;
    cfg.versionRequestTimeoutMs = 3000;
    cfg.secretRequestTimeoutMs  = 3000;
    svc.setConfig(cfg);

    std::vector<RemoteRequest> received;

    PeerValidationSendCallbacks cbs;
    cbs.sendRequest = [&](const std::string &, RemoteRequest req) { received.push_back(req); };
    svc.setSendCallbacks(cbs);

    svc.validatePeer(makeEndpoint("pc-e", "10.0.0.5"));

    ASSERT_EQ(received.size(), 2u)
        << "With both checks enabled, exactly two requests must be sent — one for version and one for secret";
    EXPECT_TRUE(std::find(received.begin(), received.end(), RemoteRequest::Version) != received.end())
        << "A RemoteRequest::Version must be among the sent requests when the version check is enabled";
    EXPECT_TRUE(std::find(received.begin(), received.end(), RemoteRequest::Secret) != received.end())
        << "A RemoteRequest::Secret must be among the sent requests when the secret check is enabled";
}

// ---------------------------------------------------------------------------
// getValidatedPeers
// ---------------------------------------------------------------------------

TEST(PeerValidationService, GetValidatedPeers_InitiallyEmpty)
{
    PeerValidationService svc;
    EXPECT_TRUE(svc.getValidatedPeers().empty())
        << "A fresh PeerValidationService must have no validated peers";
}

// ---------------------------------------------------------------------------
// onPeerDiscovered — handshake callback
// ---------------------------------------------------------------------------

TEST(PeerValidationService, OnPeerDiscovered_SendsHandshake)
{
    PeerValidationService svc;
    svc.setConfig(PeerValidationConfig{});

    std::atomic<bool> handshakeSent{false};
    std::string       capturedName;

    PeerValidationSendCallbacks cbs;
    cbs.sendHandshake = [&](const std::string &name)
    {
        capturedName = name;
        handshakeSent.store(true);
    };
    svc.setSendCallbacks(cbs);

    svc.onPeerDiscovered(makeEndpoint("pc-f", "10.0.0.6"));

    EXPECT_TRUE(handshakeSent.load())
        << "Discovering a new peer must immediately trigger an outgoing handshake via the sendHandshake callback";
    EXPECT_EQ(capturedName, "pc-f")
        << "The sendHandshake callback must receive the correct computer name of the discovered peer";
}

// ---------------------------------------------------------------------------
// clearValidatedPeer
// ---------------------------------------------------------------------------

TEST(PeerValidationService, ClearValidatedPeer_NonExistent_DoesNotCrash)
{
    PeerValidationService svc;
    EXPECT_NO_THROW(svc.clearValidatedPeer("nonexistent"))
        << "clearValidatedPeer() must not throw or crash when the given name is not in the validated peers map";
}

// ---------------------------------------------------------------------------
// Validation callback — not fired when checks are disabled
// ---------------------------------------------------------------------------

TEST(PeerValidationService, ValidationCallback_NotFired_WhenBothChecksDisabled)
{
    PeerValidationService svc;
    PeerValidationConfig  cfg;
    cfg.enableVersionCheck = false;
    cfg.enableSecretCheck  = false;
    svc.setConfig(cfg);
    svc.setSendCallbacks(makeNullCallbacks());

    std::atomic<bool> callbackFired{false};
    svc.setValidationCallback([&](const ValidationResult &) { callbackFired.store(true); });

    svc.validatePeer(makeEndpoint("pc-g", "10.0.0.7"));
    std::this_thread::sleep_for(50ms);

    EXPECT_FALSE(callbackFired.load())
        << "With both checks disabled, validatePeer() cannot reach a terminal state — the validation callback must not fire";
}

// ---------------------------------------------------------------------------
// Timeout-driven validation callbacks
// ---------------------------------------------------------------------------

TEST(PeerValidationService, VersionTimeout_Fires_WithTimedoutStatus)
{
    PeerValidationService svc;
    PeerValidationConfig  cfg;
    cfg.enableVersionCheck      = true;
    cfg.enableSecretCheck       = false;
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
    std::this_thread::sleep_for(400ms);

    EXPECT_TRUE(callbackFired.load())
        << "The validation callback must fire after the version request timeout (150 ms) elapses with no response";
    EXPECT_EQ(capturedResult.status, ValidationResult::Status::ValidationTimedout)
        << "When the version request times out the callback must report ValidationTimedout status";
}

TEST(PeerValidationService, SecretTimeout_Fires_WithTimedoutStatus)
{
    PeerValidationService svc;
    PeerValidationConfig  cfg;
    cfg.enableVersionCheck     = false;
    cfg.enableSecretCheck      = true;
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
    std::this_thread::sleep_for(400ms);

    EXPECT_TRUE(callbackFired.load())
        << "The validation callback must fire after the secret request timeout (150 ms) elapses with no response";
    EXPECT_EQ(capturedResult.status, ValidationResult::Status::ValidationTimedout)
        << "When the secret request times out the callback must report ValidationTimedout status";
}
