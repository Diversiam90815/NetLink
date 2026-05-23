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
    EXPECT_NO_THROW(svc.setConfig(PeerValidationConfig{}));
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
    EXPECT_EQ(result.status, ValidationResult::Status::ResultIncomplete);
    EXPECT_EQ(result.remoteEndpoint.displayName, "pc-a");
}

TEST(PeerValidationService, ValidatePeer_WithNullSendCallbacks_DoesNotCrash)
{
    PeerValidationService svc;
    PeerValidationConfig  cfg;
    cfg.enableVersionCheck = true;
    cfg.enableSecretCheck  = true;
    svc.setConfig(cfg);
    // No send callbacks set — must not crash inside sendRequestToRemote
    EXPECT_NO_THROW(svc.validatePeer(makeEndpoint("pc-b", "10.0.0.2")));
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

    EXPECT_TRUE(requestSent.load());
    EXPECT_EQ(capturedName, "pc-c");
    EXPECT_EQ(capturedReq, RemoteRequest::Version);
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

    EXPECT_TRUE(requestSent.load());
    EXPECT_EQ(capturedReq, RemoteRequest::Secret);
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

    ASSERT_EQ(received.size(), 2u);
    EXPECT_TRUE(std::find(received.begin(), received.end(), RemoteRequest::Version) != received.end());
    EXPECT_TRUE(std::find(received.begin(), received.end(), RemoteRequest::Secret) != received.end());
}

// ---------------------------------------------------------------------------
// getValidatedPeers
// ---------------------------------------------------------------------------

TEST(PeerValidationService, GetValidatedPeers_InitiallyEmpty)
{
    PeerValidationService svc;
    EXPECT_TRUE(svc.getValidatedPeers().empty());
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

    EXPECT_TRUE(handshakeSent.load());
    EXPECT_EQ(capturedName, "pc-f");
}

// ---------------------------------------------------------------------------
// clearValidatedPeer
// ---------------------------------------------------------------------------

TEST(PeerValidationService, ClearValidatedPeer_NonExistent_DoesNotCrash)
{
    PeerValidationService svc;
    EXPECT_NO_THROW(svc.clearValidatedPeer("nonexistent"));
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

    EXPECT_FALSE(callbackFired.load());
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

    EXPECT_TRUE(callbackFired.load());
    EXPECT_EQ(capturedResult.status, ValidationResult::Status::ValidationTimedout);
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

    EXPECT_TRUE(callbackFired.load());
    EXPECT_EQ(capturedResult.status, ValidationResult::Status::ValidationTimedout);
}
