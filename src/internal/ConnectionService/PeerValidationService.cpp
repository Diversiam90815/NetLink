/*
  ==============================================================================
	Module:         PeerValidationService
	Description:    Pre-connection peer validation with pluggable checks
  ==============================================================================
*/

#include "PeerValidationService.h"
#include "NetLinkLog.h"


void netlink::PeerValidationService::setConfig(const PeerValidationConfig &config)
{
	std::lock_guard<std::mutex> lock(mMutex);
	mConfig = config;
}


void netlink::PeerValidationService::setValidationCallback(ValidationCallback cb)
{
	std::lock_guard<std::mutex> lock(mMutex);
	mCallback = std::move(cb);
}


netlink::ValidationResult netlink::PeerValidationService::validatePeer(const DiscoveryEndpoint &peer)
{
	std::lock_guard<std::mutex> lock(mMutex);

	// Return cached result if available
	auto it = mValidatedPeers.find(peer.IPAddress);
	if (it != mValidatedPeers.end())
	{
		NETLINK_LOG_DEBUG("Returning cached validation result for {}", peer.IPAddress);
		return it->second;
	}

	ValidationResult result;
	result.remote  = peer;
	result.status  = ValidationResult::Status::ReadyForConnect;
	result.message = "Validated (no checks configured)";

	// Run enabled checks, each sets result.status on failure and returns false
	// if (mConfig.enableVersionCheck && !checkVersion(peer, result))
	// {
	//     mValidatedPeers[peer.IPAddress] = result;
	//     return result;
	// }

	NETLINK_LOG_INFO("Peer {} validated successfully", peer.IPAddress);
	mValidatedPeers[peer.IPAddress] = result;
	return result;
}


std::optional<netlink::ValidationResult> netlink::PeerValidationService::getCachedResult(const std::string &ip)
{
	std::lock_guard<std::mutex> lock(mMutex);

	auto it = mValidatedPeers.find(ip);
	if (it != mValidatedPeers.end())
		return it->second;

	return std::nullopt;
}


void netlink::PeerValidationService::clearCachedResult(const std::string &ip)
{
	std::lock_guard<std::mutex> lock(mMutex);
	mValidatedPeers.erase(ip);
}


void netlink::PeerValidationService::clearAll()
{
	std::lock_guard<std::mutex> lock(mMutex);
	mValidatedPeers.clear();
}


bool netlink::PeerValidationService::checkVersion(const DiscoveryEndpoint &peer, ValidationResult &result)
{
	// Placeholder: implement when SignalType::VersionRequest/Response are added
	(void)peer;
	(void)result;
	return true;
}
