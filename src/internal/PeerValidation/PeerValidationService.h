/*
  ==============================================================================
	Module:         PeerValidationService
	Description:    Pre-connection peer validation with pluggable checks
  ==============================================================================
*/

#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "ValidationResult.h"
#include "Discovery/DiscoveryEndpoint.h"
#include "TimeoutService/TimeoutService.h"


namespace netlink
{

enum class RemoteRequest
{
	Secret	= 1,
	Version = 2,
};

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


struct RemoteHandshake
{
	std::string							  remoteName{};
	DiscoveryEndpoint					  remoteEndpoint{};
	bool								  sent{false};
	bool								  received{false};
	std::chrono::steady_clock::time_point initiatedTime;

	bool								  isComplete() const { return sent && received; }
};


namespace PeerValidationTimouts
{
constexpr const char *Handshake		 = "handshake";
constexpr const char *VersionRequest = "version_request";
constexpr const char *SecretRequest	 = "secret_request";
} // namespace PeerValidationTimouts


class PeerValidationService
{
public:
	using ValidationCallback = std::function<void(const ValidationResult &)>;

	PeerValidationService()	 = default;
	~PeerValidationService() = default;

	// Configuration
	void			 setConfig(const PeerValidationConfig &config) { mConfig = config; }
	void			 setValidationCallback(ValidationCallback cb) { mCallback = std::move(cb); }
	void			 setSendCallbacks(PeerValidationSendCallbacks cb) { mSendCallbacks = std::move(cb); }

	// Manual validation
	ValidationResult validatePeer(const DiscoveryEndpoint &peer);
	void			 clearValidatedPeer(const std::string &computerName);

	void			 onPeerDiscovered(const DiscoveryEndpoint &remoteEndpoint);

private:
	// Validation logic
	ValidationResult						 performValidation(const std::string &computerName);

	// Pending validation
	void									 addPendingValidation(const DiscoveryEndpoint &remote);
	std::optional<PendingValidation>		 getPendingValidation(const std::string &computerName);
	void									 updatePendingValidation(const std::string &computerName, std::function<void(PendingValidation &)> updateFunc);
	void									 completePendingValidation(const std::string &computerName);
	void									 removePendingValidation(const std::string &computerName);

	// State queries
	ValidationResult						 getLastResult() const { return mLastResult; }
	std::optional<ValidationResult>			 getValidationResult(const std::string &computerName);
	void									 cancelAllPendingValidation();

	// Request handlers -> Answer incoming requests
	void									 handleSecretRequest(const std::string &computerName);
	void									 handleVersionRequest(const std::string &computerName);

	// Received requests -> Handle answers to our requests
	void									 onRequestReceived(const std::string &computerName, const RemoteRequest request);
	void									 onSecretResponseReceived(const std::string &computerName, const std::string &secret);
	void									 onVersionResponseReceived(const std::string &computerName, const std::string &version);
	void									 onHandshakeReceived(const std::string &computerName);

	// Sending requests to remote
	void									 sendRequestToRemote(const std::string &computerName, const RemoteRequest request);

	// Check compatibility
	bool									 checkVersionCompatibility(const std::string remoteVersion);
	bool									 checkSecretCompatibility(const std::string remoteSecret);

	// Handshake
	void									 sendHandshake(const std::string &computerName);
	void									 checkAndStartValidation(const std::string &computerName);

	void									 onTimeout(const TimeoutKey &key);


	PeerValidationConfig					 mConfig;
	ValidationResult						 mLastResult;

	std::string								 mLocalVersion{};
	std::string								 mLocalSecret{};

	TimeoutService							 mTimeoutService;

	ValidationCallback						 mCallback;
	PeerValidationSendCallbacks				 mSendCallbacks;

	std::map<std::string, ValidationResult>	 mValidatedPeers;	  // key = Computername
	std::mutex								 mValidatedPeerMutex;

	std::map<std::string, PendingValidation> mPendingValidations; // key = Computername
	std::mutex								 mPendingValidationMutex;

	std::map<std::string, RemoteHandshake>	 mPendingHandshakes;  // Key = Computername
	std::mutex								 mHandshakeMutex;

	std::mutex								 mStateMutex;
};

} // namespace netlink
