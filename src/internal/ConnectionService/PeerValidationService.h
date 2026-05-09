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
	void			 setValidationCallback(ValidationCallback cb) { mCallback = cb; }

	// Manual validation
	ValidationResult validatePeer(const DiscoveryEndpoint &peer);
	void			 clearValidatedPeer(const std::string &computerName);

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

	std::string								 localVersion{};
	std::string								 localSecret{};

	TimeoutService							 mTimeoutService;

	ValidationCallback						 mCallback;

	std::map<std::string, ValidationResult>	 mValidatedPeers;	  // key = Computername
	std::mutex								 mValidatedPeerMutex;

	std::map<std::string, PendingValidation> mPendingValidations; // key = Computername
	std::mutex								 mPendingValidationMutex;

	std::map<std::string, RemoteHandshake>	 mPendingHandshakes;  // Key = Computername
	std::mutex								 mHandshakeMutex;

	std::mutex								 mStateMutex;
};

} // namespace netlink
