/*
  ==============================================================================
	Module:         PeerValidationService
	Description:    Pre-connection peer validation orchestrator
  ==============================================================================
*/

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ValidationResult.h"
#include "PendingValidationStore.h"
#include "ValidatedPeerRegistry.h"
#include "HandshakeTracker.h"
#include "Checks/ICompatibilityCheck.h"
#include "Discovery/DiscoveryEndpoint.h"
#include "TimeoutService/TimeoutService.h"


namespace netlink
{

struct PeerValidationConfig
{
	bool enableVersionCheck{false};
	bool enableSecretCheck{false};
	int	 handshakeTimeoutMs{3000};
	int	 versionRequestTimeoutMs{3000};
	int	 secretRequestTimeoutMs{3000};
};


struct PeerValidationSendCallbacks
{
	// outgoing validation messages via SignalingService
	std::function<void(const std::string &computerName, RemoteRequest)>			 sendRequest;
	std::function<void(const std::string &computerName, const std::string &val)> sendSecretResponse;
	std::function<void(const std::string &computerName, const std::string &val)> sendVersionResponse;
	std::function<void(const std::string &computerName)>						 sendHandshake;
};


namespace PeerValidationTimouts
{
constexpr const char *Handshake = "handshake";
} // namespace PeerValidationTimouts


class PeerValidationService
{
public:
	using ValidationCallback = std::function<void(const ValidationResult &)>;

	PeerValidationService();
	~PeerValidationService() = default;

	// Configuration
	void							setConfig(const PeerValidationConfig &config);
	void							setLocalSecret(const std::string &secret);
	void							setLocalVersion(const std::string &version);
	void							setValidationCallback(ValidationCallback cb) { mCallback = std::move(cb); }
	void							setSendCallbacks(PeerValidationSendCallbacks cb) { mSendCallbacks = std::move(cb); }

	// Manual validation
	ValidationResult				validatePeer(const DiscoveryEndpoint &peer);
	void							clearValidatedPeer(const std::string &computerName);

	void							onPeerDiscovered(const DiscoveryEndpoint &remoteEndpoint);

	std::vector<ValidationResult>	getValidatedPeers();

	// Incoming remote events (called by the signaling layer)
	void							onRequestReceived(const std::string &computerName, RemoteRequest request);
	void							onCheckResponseReceived(const std::string &computerName, RemoteRequest request, const std::string &value);
	void							onHandshakeReceived(const std::string &computerName);

	// State queries
	ValidationResult				getLastResult() const;
	std::optional<ValidationResult> getValidationResult(const std::string &computerName);
	void							cancelAllPendingValidation();

private:
	// Validation logic - evaluates all registered checks concurrently
	ValidationResult								  performValidation(const std::string &computerName);

	void											  completePendingValidation(const std::string &computerName);
	bool											  allChecksReady(const std::string &computerName);

	// Request handlers -> Answer incoming requests
	void											  handleSecretRequest(const std::string &computerName);
	void											  handleVersionRequest(const std::string &computerName);

	// Sending requests to remote
	void											  sendRequestToRemote(const std::string &computerName, ICompatibilityCheck &check);

	// Handshake
	void											  sendHandshake(const std::string &computerName);
	void											  checkAndStartValidation(const std::string &computerName);

	void											  onTimeout(const TimeoutKey &key);

	// Rebuild mChecks from current config/secret/version
	void											  rebuildChecks();
	ICompatibilityCheck								 *findCheck(RemoteRequest wireType);


	PeerValidationConfig							  mConfig;
	ValidationResult								  mLastResult;
	mutable std::mutex								  mStateMutex;

	std::string										  mLocalVersion{};
	std::string										  mLocalSecret{};

	TimeoutService									  mTimeoutService;

	ValidationCallback								  mCallback;
	PeerValidationSendCallbacks						  mSendCallbacks;

	std::vector<std::unique_ptr<ICompatibilityCheck>> mChecks;
	std::mutex										  mChecksMutex;

	ValidatedPeerRegistry							  mValidatedPeers;
	PendingValidationStore							  mPendingValidations;
	HandshakeTracker								  mHandshakes;
};

} // namespace netlink
