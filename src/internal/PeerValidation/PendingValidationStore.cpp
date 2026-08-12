/*
  ==============================================================================
	Module:         PendingValidationStore
	Description:    Thread-safe storage for in-flight peer validations
  ==============================================================================
*/

#include "PendingValidationStore.h"
#include "NetLinkLog.h"


void netlink::PendingValidationStore::add(const DiscoveryEndpoint &remote)
{
	std::lock_guard<std::mutex> lock(mMutex);

	PendingValidation			pending;
	pending.computerName		 = remote.displayName;
	pending.IPv4				 = remote.IPAddress;
	pending.remoteEndpoint		 = remote;
	pending.requestTime			 = std::chrono::steady_clock::now();

	mPending[remote.displayName] = pending;

	NETLINK_LOG_DEBUG("Added pending validation for {}", remote.displayName);
}


std::optional<netlink::PendingValidation> netlink::PendingValidationStore::get(const std::string &computerName) const
{
	std::lock_guard<std::mutex> lock(mMutex);

	auto						it = mPending.find(computerName);
	if (it != mPending.end())
		return it->second;

	return std::nullopt;
}


void netlink::PendingValidationStore::remove(const std::string &computerName)
{
	std::lock_guard<std::mutex> lock(mMutex);
	mPending.erase(computerName);

	NETLINK_LOG_DEBUG("Removed pending validation for {}", computerName);
}


void netlink::PendingValidationStore::clear()
{
	std::lock_guard<std::mutex> lock(mMutex);
	mPending.clear();
}
