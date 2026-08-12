/*
  ==============================================================================
	Module:         ValidatedPeerRegistry
	Description:    Thread-safe cache of completed peer validation results
  ==============================================================================
*/


#include "ValidatedPeerRegistry.h"


void netlink::ValidatedPeerRegistry::store(const std::string &computerName, const ValidationResult &result)
{
	std::lock_guard<std::mutex> lock(mMutex);
	mValidatedPeers[computerName] = result;
}


std::optional<netlink::ValidationResult> netlink::ValidatedPeerRegistry::get(const std::string &computerName) const
{
	std::lock_guard<std::mutex> lock(mMutex);

	auto						it = mValidatedPeers.find(computerName);
	if (it != mValidatedPeers.end())
		return it->second;

	return std::nullopt;
}


void netlink::ValidatedPeerRegistry::remove(const std::string &computerName)
{
	std::lock_guard<std::mutex> lock(mMutex);
	mValidatedPeers.erase(computerName);
}


std::vector<netlink::ValidationResult> netlink::ValidatedPeerRegistry::getAllReadyToConnect() const
{
	std::lock_guard<std::mutex>	  lock(mMutex);

	std::vector<ValidationResult> result;
	result.reserve(mValidatedPeers.size());

	for (const auto &[name, vr] : mValidatedPeers)
	{
		if (vr.isReadyToConnect())
			result.push_back(vr);
	}

	return result;
}
