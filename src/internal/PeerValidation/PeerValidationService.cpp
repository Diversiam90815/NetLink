/*
  ==============================================================================
	Module:         PeerValidationService
	Description:    Pre-connection peer validation orchestrator
  ==============================================================================
*/


#include <future>

#include "PeerValidationService.h"
#include "Checks/SecretCompatibilityCheck.h"
#include "Checks/VersionCompatibilityCheck.h"
#include "NetLinkLog.h"


netlink::PeerValidationService::PeerValidationService()
{
	rebuildChecks();
}


void netlink::PeerValidationService::setConfig(const PeerValidationConfig &config)
{
	mConfig = config;
	rebuildChecks();
}


void netlink::PeerValidationService::setLocalSecret(const std::string &secret)
{
	mLocalSecret = secret;
	rebuildChecks();
}


void netlink::PeerValidationService::setLocalVersion(const std::string &version)
{
	mLocalVersion = version;
	rebuildChecks();
}


netlink::ValidationResult netlink::PeerValidationService::validatePeer(const DiscoveryEndpoint &peer)
{
	std::lock_guard<std::mutex> lock(mStateMutex);

	ValidationResult			result;
	result.remoteEndpoint = peer;
	result.canConnect	  = false;
	result.needsAction	  = false;

	// Check if already validated
	if (auto existing = mValidatedPeers.get(peer.displayName))
	{
		NETLINK_LOG_INFO("Peer {} already validated.", peer.displayName);
		result		  = *existing;
		result.status = ValidationResult::Status::AlreadyValidated;
		return result;
	}

	// start async validation by adding pending validation
	mPendingValidations.add(peer);

	// kick off every registered check concurrently
	{
		std::lock_guard<std::mutex> checksLock(mChecksMutex);

		for (auto &check : mChecks)
		{
			if (check->requiresRemoteRequest())
			{
				NETLINK_LOG_INFO("Requesting {} from {}", check->name(), peer.displayName);
				sendRequestToRemote(peer.displayName, *check);
			}
		}
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

	mValidatedPeers.remove(computerName);
	mHandshakes.remove(computerName);
}


void netlink::PeerValidationService::onPeerDiscovered(const DiscoveryEndpoint &remoteEndpoint)
{
	NETLINK_LOG_INFO("Found a remote peer : {}", remoteEndpoint.displayName);

	bool startedNewHandshake = mHandshakes.beginForDiscoveredPeer(remoteEndpoint);

	if (startedNewHandshake)
	{
		TimeoutKey key{PeerValidationTimouts::Handshake, remoteEndpoint.displayName};
		mTimeoutService.startTimeout(key, mConfig.handshakeTimeoutMs, [this](const TimeoutKey &key) { onTimeout(key); });
	}

	sendHandshake(remoteEndpoint.displayName);
}


std::vector<netlink::ValidationResult> netlink::PeerValidationService::getValidatedPeers()
{
	return mValidatedPeers.getAllReadyToConnect();
}


netlink::ValidationResult netlink::PeerValidationService::performValidation(const std::string &computerName)
{
	ValidationResult result;

	auto			 pending = mPendingValidations.get(computerName);
	if (!pending)
	{
		NETLINK_LOG_ERROR("No pending validation found for {}", computerName);
		result.status  = ValidationResult::Status::PeerInvalid;
		result.message = "Internal error: no pending validation";
		return result;
	}

	result.remoteEndpoint = pending->remoteEndpoint;

	// Evaluate every registered check concurrently
	std::vector<std::pair<ICompatibilityCheck *, std::future<bool>>> futures;

	{
		std::lock_guard<std::mutex> lock(mChecksMutex);
		futures.reserve(mChecks.size());

		for (auto &check : mChecks)
		{
			ICompatibilityCheck *checkPtr = check.get();
			futures.emplace_back(checkPtr, std::async(std::launch::async, [checkPtr, computerName] { return checkPtr->evaluate(computerName); }));
		}
	}

	ICompatibilityCheck *firstFailure = nullptr;

	for (auto &[check, future] : futures)
	{
		bool passed = future.get();

		if (!passed && firstFailure == nullptr)
			firstFailure = check;
	}

	// Pull remote version (if that check ran) for reporting purposes, regardless of pass/fail
	if (auto *versionCheck = dynamic_cast<VersionCompatibilityCheck *>(findCheck(RemoteRequest::Version)))
		result.remoteVersion = versionCheck->remoteVersion(computerName);

	if (firstFailure != nullptr)
	{
		result.status	  = firstFailure->name() == "secret_request" ? ValidationResult::Status::SecretMissmatch : ValidationResult::Status::VersionMissmatch;
		result.message	  = firstFailure->failureMessage(computerName);
		result.canConnect = false;
		return result;
	}

	// all checks passed
	result.status	   = ValidationResult::Status::ReadyToConnect;
	result.message	   = "Peer validated and is ready to connect";
	result.canConnect  = true;
	result.needsAction = false;

	NETLINK_LOG_INFO("Peer successfully validated: {}", computerName);
	return result;
}


void netlink::PeerValidationService::completePendingValidation(const std::string &computerName)
{
	auto pending = mPendingValidations.get(computerName);

	if (!pending)
	{
		NETLINK_LOG_WARNING("No pending validation to complete for {}", computerName);
		return;
	}

	NETLINK_LOG_INFO("Completing validation for {}", computerName);

	// perform final validation
	auto result = performValidation(computerName);

	mValidatedPeers.store(computerName, result);

	{
		std::lock_guard<std::mutex> lock(mStateMutex);
		mLastResult = result;
	}

	// notify
	if (mCallback)
		mCallback(result);

	// remove from pending
	mPendingValidations.remove(computerName);

	// Reset per-peer state on every check so a future re-validation starts clean
	{
		std::lock_guard<std::mutex> lock(mChecksMutex);
		for (auto &check : mChecks)
			check->reset(computerName);
	}

	NETLINK_LOG_INFO("Validation completed for {} with status {}", computerName, static_cast<int>(result.status));
}


bool netlink::PeerValidationService::allChecksReady(const std::string &computerName)
{
	std::lock_guard<std::mutex> checksLock(mChecksMutex);

	for (const auto &check : mChecks)
	{
		if (!check->isReady(computerName))
			return false;
	}

	return true;
}


netlink::ValidationResult netlink::PeerValidationService::getLastResult() const
{
	std::lock_guard<std::mutex> lock(mStateMutex);
	return mLastResult;
}


std::optional<netlink::ValidationResult> netlink::PeerValidationService::getValidationResult(const std::string &computerName)
{
	return mValidatedPeers.get(computerName);
}


void netlink::PeerValidationService::cancelAllPendingValidation()
{
	mPendingValidations.clear();
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


void netlink::PeerValidationService::sendRequestToRemote(const std::string &computerName, ICompatibilityCheck &check)
{
	auto wireType = check.wireRequestType();
	if (!wireType.has_value())
		return; // check has its own means of gathering data (e.g. purely local)

	int timeoutMS = 0;

	switch (*wireType)
	{
	case RemoteRequest::Secret: timeoutMS = mConfig.secretRequestTimeoutMs; break;
	case RemoteRequest::Version: timeoutMS = mConfig.versionRequestTimeoutMs; break;
	}

	NETLINK_LOG_DEBUG("Sending request {} to {}", check.name(), computerName);

	if (mSendCallbacks.sendRequest)
		mSendCallbacks.sendRequest(computerName, *wireType);

	if (timeoutMS > 0)
	{
		TimeoutKey key{check.name(), computerName};
		mTimeoutService.startTimeout(key, timeoutMS, [this](const TimeoutKey &key) { onTimeout(key); });
	}
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


void netlink::PeerValidationService::onCheckResponseReceived(const std::string &computerName, RemoteRequest request, const std::string &value)
{
	ICompatibilityCheck *check = findCheck(request);

	if (check == nullptr)
	{
		NETLINK_LOG_WARNING("Received response for unknown/disabled check ({}) from {}", static_cast<int>(request), computerName);
		return;
	}

	NETLINK_LOG_INFO("Received {} answer from {}", check->name(), computerName);

	mTimeoutService.cancelTimeout({check->name(), computerName});

	check->onRemoteDataReceived(computerName, value);

	if (allChecksReady(computerName))
		completePendingValidation(computerName);
}


void netlink::PeerValidationService::onHandshakeReceived(const std::string &computerName)
{
	NETLINK_LOG_INFO("Received remote handshake from {}", computerName);

	bool	   isNewHandshake = mHandshakes.markReceived(computerName);

	TimeoutKey key			  = {PeerValidationTimouts::Handshake, computerName};

	if (isNewHandshake)
		mTimeoutService.startTimeout(key, mConfig.handshakeTimeoutMs, [this](const TimeoutKey &key) { onTimeout(key); });
	else
		mTimeoutService.cancelTimeout(key);

	checkAndStartValidation(computerName);
}


void netlink::PeerValidationService::sendHandshake(const std::string &computerName)
{
	NETLINK_LOG_INFO("Sending handshake to {}", computerName);

	mHandshakes.markSent(computerName);

	if (mSendCallbacks.sendHandshake)
		mSendCallbacks.sendHandshake(computerName);

	checkAndStartValidation(computerName);
}


void netlink::PeerValidationService::checkAndStartValidation(const std::string &computerName)
{
	auto remoteEndpoint = mHandshakes.tryCompleteAndRemove(computerName);

	if (!remoteEndpoint.has_value())
		return; // handshake still incomplete (or unknown)

	mTimeoutService.cancelTimeout({PeerValidationTimouts::Handshake, computerName});

	if (!remoteEndpoint->isEmpty())
		validatePeer(*remoteEndpoint);
}


void netlink::PeerValidationService::onTimeout(const TimeoutKey &key)
{
	NETLINK_LOG_WARNING("Timeout expired: {}", key.toString());

	if (key.category == PeerValidationTimouts::Handshake)
	{
		mHandshakes.remove(key.identifier);
		NETLINK_LOG_WARNING("Handshake timed out for {}", key.identifier);
		return;
	}

	// otherwise this must be one of the compatibility check timeouts - fail entire validation
	auto pending = mPendingValidations.get(key.identifier);

	if (!pending)
		return;

	ValidationResult result;
	result.status		  = ValidationResult::Status::ValidationTimedout;
	result.message		  = "Validation timed out waiting for " + key.category;
	result.remoteEndpoint = pending->remoteEndpoint;
	result.canConnect	  = false;
	result.needsAction	  = false;

	if (mCallback)
		mCallback(result);

	mPendingValidations.remove(key.identifier);
	mTimeoutService.cancelByIdentifier(key.identifier);

	{
		std::lock_guard<std::mutex> lock(mChecksMutex);
		for (auto &check : mChecks)
			check->reset(key.identifier);
	}

	NETLINK_LOG_INFO("Validation failed due to timeout for {}", key.identifier);
}


void netlink::PeerValidationService::rebuildChecks()
{
	std::lock_guard<std::mutex> lock(mChecksMutex);

	mChecks.clear();

	if (mConfig.enableSecretCheck)
		mChecks.push_back(std::make_unique<SecretCompatibilityCheck>(mLocalSecret));

	if (mConfig.enableVersionCheck)
		mChecks.push_back(std::make_unique<VersionCompatibilityCheck>(mLocalVersion));
}


netlink::ICompatibilityCheck *netlink::PeerValidationService::findCheck(RemoteRequest wireType)
{
	std::lock_guard<std::mutex> lock(mChecksMutex);

	for (auto &check : mChecks)
	{
		if (check->wireRequestType() == wireType)
			return check.get();
	}

	return nullptr;
}
