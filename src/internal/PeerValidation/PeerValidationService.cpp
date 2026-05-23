/*
/*
  ==============================================================================
	Module:         PeerValidationService
	Description:    Pre-connection peer validation with pluggable checks
  ==============================================================================
*/

#include "PeerValidationService.h"
#include "NetLinkLog.h"



netlink::ValidationResult netlink::PeerValidationService::validatePeer(const DiscoveryEndpoint &peer)
{
	std::lock_guard<std::mutex> lock(mStateMutex);

	ValidationResult			result;
	result.remoteEndpoint = peer;
	result.canConnect	  = false;
	result.needsAction	  = false;

	// Check if already validated
	{
		std::lock_guard<std::mutex> lock(mValidatedPeerMutex);
		auto						it = mValidatedPeers.find(result.remoteEndpoint.displayName);

		if (it != mValidatedPeers.end())
		{
			NETLINK_LOG_INFO("Peer {} already validated.", result.remoteEndpoint.displayName);
			result		  = it->second;
			result.status = ValidationResult::Status::AlreadyValidated;
			return result;
		}
	}

	// start async validation by adding pending validation
	addPendingValidation(peer);

	// 1. Request remote secret
	if (mConfig.enableSecretCheck)
	{
		NETLINK_LOG_INFO("Requesting secret from {}", peer.displayName);
		sendRequestToRemote(peer.displayName, RemoteRequest::Secret);
	}
	else
	{
		// mark secret as received and matching if check is disabled
		updatePendingValidation(peer.displayName,
								[this](PendingValidation &pending)
								{
									pending.secretReceived = true;
									pending.secret		   = mLocalSecret;
								});
	}

	// 2. Request remote version
	if (mConfig.enableVersionCheck)
	{
		NETLINK_LOG_INFO("Requesting version from {}", peer.displayName);
		sendRequestToRemote(peer.displayName, RemoteRequest::Version);
	}
	else
	{
		// mark version as received and matching if check is disabled
		updatePendingValidation(peer.displayName,
								[this](PendingValidation &pending)
								{
									pending.versionReceived = true;
									pending.version			= mLocalVersion;
								});
	}

	// Return early result
	result.status  = ValidationResult::Status::ResultIncomplete;
	result.message = "Validation in progress";
	mLastResult	   = result;

	return result;
}


void netlink::PeerValidationService::clearValidatedPeer(const std::string &computerName)
{
	NETLINK_LOG_INFO("Clearing validated peer cache for {}", computerName);

	{
		std::lock_guard<std::mutex> lock(mValidatedPeerMutex);
		mValidatedPeers.erase(computerName);
	}

	{
		std::lock_guard<std::mutex> lock(mHandshakeMutex);
		mPendingHandshakes.erase(computerName);
	}
}


void netlink::PeerValidationService::onPeerDiscovered(const DiscoveryEndpoint &remoteEndpoint)
{
	NETLINK_LOG_INFO("Found a remote peer : {}", remoteEndpoint.displayName);

	bool startedNewHandshake = false;

	// Track handshake initiation
	{
		std::lock_guard<std::mutex> lock(mHandshakeMutex);
		auto						it = mPendingHandshakes.find(remoteEndpoint.displayName);

		if (it == mPendingHandshakes.end())
		{
			// There is no handshake yet, so we create one
			RemoteHandshake handshake;
			handshake.remoteName						   = remoteEndpoint.displayName;
			handshake.remoteEndpoint					   = remoteEndpoint;
			handshake.sent								   = false;
			handshake.received							   = false;
			handshake.initiatedTime						   = std::chrono::steady_clock::now();

			mPendingHandshakes[remoteEndpoint.displayName] = handshake;
			startedNewHandshake							   = true;
		}
		else
		{
			// update handshake with remote endpoint (it was received when receiving a handshake without the endpoint info
			it->second.remoteEndpoint = remoteEndpoint;

			NETLINK_LOG_DEBUG("Updated existing handshake for {} with RemoteEndpoint", remoteEndpoint.displayName);
		}
	}

	if (startedNewHandshake)
	{
		TimeoutKey key{PeerValidationTimouts::Handshake, remoteEndpoint.displayName};
		mTimeoutService.startTimeout(key, mConfig.handshakeTimeoutMs, [this](const TimeoutKey &key) { onTimeout(key); });
	}

	// send the handshake
	sendHandshake(remoteEndpoint.displayName);
}


std::vector<netlink::ValidationResult> netlink::PeerValidationService::getValidatedPeers()
{
	std::lock_guard<std::mutex>	  lock(mValidatedPeerMutex);

	std::vector<ValidationResult> result;
	result.reserve(mValidatedPeers.size());

	for (const auto &[name, vr] : mValidatedPeers)
	{
		if (vr.isReadyToConnect())
			result.push_back(vr);
	}

	return result;
}


netlink::ValidationResult netlink::PeerValidationService::performValidation(const std::string &computerName)
{
	ValidationResult result;

	// get pending validation data
	auto			 pending = getPendingValidation(computerName);
	if (!pending)
	{
		NETLINK_LOG_ERROR("No pending validation found for {}", computerName);
		result.status  = ValidationResult::Status::PeerInvalid;
		result.message = "Internal error: no pending validation";
		return result;
	}

	result.remoteEndpoint = pending->remoteEndpoint;

	// 1. validate secret
	if (mConfig.enableSecretCheck)
	{
		bool secretCompatible = checkSecretCompatibility(pending->secret);

		if (!secretCompatible)
		{
			// secret is not compatible
			result.status	  = ValidationResult::Status::SecretMissmatch;
			result.message	  = "Secret missmatch";
			result.canConnect = false;
			return result;
		}
	}

	// 2. check version
	if (mConfig.enableVersionCheck)
	{
		bool versionCompatible = checkVersionCompatibility(pending->version);

		if (!versionCompatible)
		{
			result.status		 = ValidationResult::Status::VersionMissmatch;
			result.message		 = "Version missmatch";
			result.remoteVersion = pending->version;
			result.canConnect	 = false;
			return result;
		}
	}

	// all checks have passed
	result.status	   = ValidationResult::Status::ReadyToConnect;
	result.message	   = "Peer validated and is ready to connect";
	result.canConnect  = true;
	result.needsAction = false;

	NETLINK_LOG_INFO("Peer successfully validated: {}", computerName);
	return result;
}


void netlink::PeerValidationService::addPendingValidation(const DiscoveryEndpoint &remote)
{
	std::lock_guard<std::mutex> lock(mPendingValidationMutex);

	PendingValidation			pending;
	pending.computerName					= remote.displayName;
	pending.IPv4							= remote.IPAddress;
	pending.remoteEndpoint					= remote;
	pending.requestTime						= std::chrono::steady_clock::now();

	mPendingValidations[remote.displayName] = pending;

	NETLINK_LOG_DEBUG("Added pending validation for {}", remote.displayName);
}


std::optional<netlink::PendingValidation> netlink::PeerValidationService::getPendingValidation(const std::string &computerName)
{
	std::lock_guard<std::mutex> lock(mPendingValidationMutex);

	auto						it = mPendingValidations.find(computerName);

	if (it != mPendingValidations.end())
		return it->second;

	return std::nullopt;
}


void netlink::PeerValidationService::updatePendingValidation(const std::string &computerName, std::function<void(PendingValidation &)> updateFunc)
{
	std::lock_guard<std::mutex> lock(mPendingValidationMutex);

	auto						it = mPendingValidations.find(computerName);

	if (it != mPendingValidations.end())
	{
		updateFunc(it->second);
		NETLINK_LOG_DEBUG("Updated pending validation for {}", computerName);
	}
}


void netlink::PeerValidationService::completePendingValidation(const std::string &computerName)
{
	auto pending = getPendingValidation(computerName);

	if (!pending)
	{
		NETLINK_LOG_WARNING("No pending validation to complete for {}", computerName);
		return;
	}

	NETLINK_LOG_INFO("Completing validation for {}", computerName);

	// perform final validation
	auto result = performValidation(computerName);

	// store result
	{
		std::lock_guard<std::mutex> lock(mValidatedPeerMutex);
		mValidatedPeers[computerName] = result;
	}

	// store the last result
	mLastResult = result;

	// notify
	if (mCallback)
		mCallback(result);

	// remove from pending
	removePendingValidation(computerName);

	NETLINK_LOG_INFO("Validation completed for {} with status {}", computerName, static_cast<int>(result.status));
}


void netlink::PeerValidationService::removePendingValidation(const std::string &computerName)
{
	std::lock_guard<std::mutex> lock(mPendingValidationMutex);

	mPendingValidations.erase(computerName);
	NETLINK_LOG_DEBUG("Removed pending validation for {}", computerName);
}


std::optional<netlink::ValidationResult> netlink::PeerValidationService::getValidationResult(const std::string &computerName)
{
	std::lock_guard<std::mutex> lock(mValidatedPeerMutex);

	auto						it = mValidatedPeers.find(computerName);

	if (it != mValidatedPeers.end())
		return it->second;

	return std::nullopt;
}


void netlink::PeerValidationService::cancelAllPendingValidation()
{
	{
		std::lock_guard<std::mutex> lock(mPendingValidationMutex);
		mPendingValidations.clear();
	}

	mTimeoutService.cancelAll();
}



void netlink::PeerValidationService::handleSecretRequest(const std::string &computerName)
{
	NETLINK_LOG_INFO("Handling secret request from {}", computerName);

	// Send our secret to the remote
	if (mSendCallbacks.sendSecretResponse)
		mSendCallbacks.sendSecretResponse(computerName, mLocalSecret);

	NETLINK_LOG_DEBUG("Sent our secret answer to {}", computerName);
}


void netlink::PeerValidationService::handleVersionRequest(const std::string &computerName)
{
	NETLINK_LOG_INFO("Handling version request from {}", computerName);

	// send our version to the remote
	if (mSendCallbacks.sendVersionResponse)
		mSendCallbacks.sendVersionResponse(computerName, mLocalVersion);

	NETLINK_LOG_DEBUG("Sent our version answer to {}", computerName);
}


void netlink::PeerValidationService::onRequestReceived(const std::string &computerName, const RemoteRequest request)
{
	switch (request)
	{
	case RemoteRequest::Secret: handleSecretRequest(computerName); break;
	case RemoteRequest::Version: handleVersionRequest(computerName); break;
	default: NETLINK_LOG_WARNING("Unknown request type : {}", static_cast<int>(request)); break;
	}
}


void netlink::PeerValidationService::onSecretResponseReceived(const std::string &computerName, const std::string &secret)
{
	NETLINK_LOG_INFO("Received secret answer from {}", computerName);

	// cancel timeout
	mTimeoutService.cancelTimeout({PeerValidationTimouts::SecretRequest, computerName});

	std::lock_guard<std::mutex> lock(mStateMutex);

	updatePendingValidation(computerName,
							[&secret](PendingValidation &pending)
							{
								pending.secretReceived = true;
								pending.secret		   = secret;
							});

	// check if this completes the validation
	auto pending = getPendingValidation(computerName);

	if (pending && pending->isComplete())
		completePendingValidation(computerName);
}


void netlink::PeerValidationService::onVersionResponseReceived(const std::string &computerName, const std::string &version)
{
	NETLINK_LOG_INFO("Received version answer from {}", computerName);

	// cancel timeout
	mTimeoutService.cancelTimeout({PeerValidationTimouts::VersionRequest, computerName});

	std::lock_guard<std::mutex> lock(mStateMutex);

	updatePendingValidation(computerName,
							[&version](PendingValidation &pending)
							{
								pending.versionReceived = true;
								pending.version			= version;
							});

	// check if this completes the validation
	auto pending = getPendingValidation(computerName);

	if (pending && pending->isComplete())
		completePendingValidation(computerName);
}


void netlink::PeerValidationService::onHandshakeReceived(const std::string &computerName)
{
	NETLINK_LOG_INFO("Received remote handshake from {}", computerName);

	bool isNewHandshake = false;

	{
		std::lock_guard<std::mutex> lock(mHandshakeMutex);
		auto						it = mPendingHandshakes.find(computerName);

		if (it != mPendingHandshakes.end())
			it->second.received = true;
		else
		{
			// remote sent handshake before we discovered them
			RemoteHandshake handshake;
			handshake.remoteName			 = computerName;
			handshake.received				 = true;
			handshake.sent					 = false;
			handshake.initiatedTime			 = std::chrono::steady_clock::now();

			mPendingHandshakes[computerName] = handshake;
			isNewHandshake					 = true;
		}
	}

	// Start timeout for new handshake
	if (isNewHandshake)
	{
		TimeoutKey key = {PeerValidationTimouts::Handshake, computerName};
		mTimeoutService.startTimeout(key, mConfig.handshakeTimeoutMs, [this](const TimeoutKey &key) { onTimeout(key); });
	}
	else
	{
		// this is not a new handshake, so we stop a potential timer
		TimeoutKey key = {PeerValidationTimouts::Handshake, computerName};
		mTimeoutService.cancelTimeout(key);
	}

	// check if handshake is complete and start validation
	checkAndStartValidation(computerName);
}


void netlink::PeerValidationService::sendRequestToRemote(const std::string &computerName, const RemoteRequest request)
{
	// create timeout for the request
	int		   timeoutMS = 0;
	TimeoutKey key;

	switch (request)
	{
	case RemoteRequest::Secret:
		key		  = {PeerValidationTimouts::SecretRequest, computerName};
		timeoutMS = mConfig.secretRequestTimeoutMs;
		break;
	case RemoteRequest::Version:
		key		  = {PeerValidationTimouts::VersionRequest, computerName};
		timeoutMS = mConfig.versionRequestTimeoutMs;
		break;
	}

	NETLINK_LOG_DEBUG("Sending request {} to {}", static_cast<int>(request), computerName);

	// send request
	if (mSendCallbacks.sendRequest)
		mSendCallbacks.sendRequest(computerName, request);

	if (timeoutMS > 0)
		mTimeoutService.startTimeout(key, timeoutMS, [this](const TimeoutKey &key) { onTimeout(key); });
}


bool netlink::PeerValidationService::checkVersionCompatibility(const std::string remoteVersion)
{
	return mLocalVersion == remoteVersion;
}


bool netlink::PeerValidationService::checkSecretCompatibility(const std::string remoteSecret)
{
	return mLocalSecret == remoteSecret;
}


void netlink::PeerValidationService::sendHandshake(const std::string &computerName)
{
	NETLINK_LOG_INFO("Sending handshake to {}", computerName);

	// mark that we sent handshake
	{
		std::lock_guard<std::mutex> lock(mHandshakeMutex);

		auto						it = mPendingHandshakes.find(computerName);

		if (it != mPendingHandshakes.end())
			it->second.sent = true;
	}

	// send handshake
	if (mSendCallbacks.sendHandshake)
		mSendCallbacks.sendHandshake(computerName);

	// check if handshake is complete
	checkAndStartValidation(computerName);
}


void netlink::PeerValidationService::checkAndStartValidation(const std::string &computerName)
{
	bool			  shouldStartValidation = false;
	DiscoveryEndpoint remoteEndpoint;

	{
		std::lock_guard<std::mutex> lock(mHandshakeMutex);

		auto						it = mPendingHandshakes.find(computerName);

		if (it != mPendingHandshakes.end())
		{
			NETLINK_LOG_INFO("Remote Handshake complete with {}. Starting validation..", computerName);
			shouldStartValidation = true;

			// Get peer endpoint
			remoteEndpoint		  = it->second.remoteEndpoint;

			// Cleanup handshake tracking
			mPendingHandshakes.erase(it);
		}
	}

	if (shouldStartValidation)
	{
		// Cancel handshake timeout
		mTimeoutService.cancelTimeout({PeerValidationTimouts::Handshake, computerName});

		if (!remoteEndpoint.isEmpty())
			validatePeer(remoteEndpoint);
	}
	else
	{
		std::lock_guard<std::mutex> lock(mHandshakeMutex);

		auto						it = mPendingHandshakes.find(computerName);

		if (it != mPendingHandshakes.end())
			NETLINK_LOG_DEBUG("Handshake incomplete for {} : sent: {}, received: {}. Waiting..", computerName, it->second.sent ? "true" : "false",
							  it->second.received ? "true" : "false");
		else
			NETLINK_LOG_WARNING("Cannot start validation for {}. No pending handshake found!", computerName);
	}
}


void netlink::PeerValidationService::onTimeout(const TimeoutKey &key)
{
	NETLINK_LOG_WARNING("Timeout expired: {}", key.toString());

	if (key.category == PeerValidationTimouts::Handshake)
	{
		// handshake timedout -> clean up handshake tracking
		{
			std::lock_guard<std::mutex> lock(mHandshakeMutex);
			mPendingHandshakes.erase(key.identifier);
		}

		NETLINK_LOG_WARNING("Handshake timed out for {}", key.identifier);
	}
	else if (key.category == PeerValidationTimouts::SecretRequest || key.category == PeerValidationTimouts::VersionRequest)
	{
		// a request timed out -> fail entire validation for peer
		auto pending = getPendingValidation(key.identifier);

		if (!pending)
			return;

		// Create timedout result and notify
		ValidationResult result;
		result.status		  = ValidationResult::Status::ValidationTimedout;
		result.message		  = "Validation timed out waiting for " + key.category;
		result.remoteEndpoint = pending->remoteEndpoint;
		result.canConnect	  = false;
		result.needsAction	  = false;

		// notify
		if (mCallback)
			mCallback(result);

		// Remove pending validation
		removePendingValidation(key.identifier);

		// cancel any tother pending timeouts for this peer
		mTimeoutService.cancelByIdentifier(key.identifier);

		NETLINK_LOG_INFO("Validation failed due to timeout for {}", key.identifier);
	}
}
