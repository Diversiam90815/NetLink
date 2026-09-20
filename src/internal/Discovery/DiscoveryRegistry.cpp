/*
  ==============================================================================
	Module:         DiscoveryRegistry
	Description:    Bookkeeping of remote known peers
  ==============================================================================
*/

#include "DiscoveryRegistry.h"


namespace netlink::discovery
{

auto DiscoveryRegistry::findIt(const DiscoveryEndpoint &endpoint)
{
	return std::find_if(mKnownPeers.begin(), mKnownPeers.end(), [&](const KnownPeer &peer) { return peer.endpoint == endpoint; });
}


auto DiscoveryRegistry::findIt(const DiscoveryEndpoint &endpoint) const
{
	return std::find_if(mKnownPeers.begin(), mKnownPeers.end(), [&](const KnownPeer &peer) { return peer.endpoint == endpoint; });
}


DiscoveryRegistry::UpdateResult DiscoveryRegistry::addOrUpdate(const DiscoveryEndpoint &endpoint, TimePoint now)
{
	std::lock_guard lock(mMutex);

	auto			it = findIt(endpoint);

	if (it != mKnownPeers.end())
	{
		it->endpoint = endpoint; // refresh metadata
		it->lastSeen = now;
		return UpdateResult::Updated;
	}

	mKnownPeers.push_back(KnownPeer{endpoint, now});
	return UpdateResult::Added;
}


bool DiscoveryRegistry::touch(const DiscoveryEndpoint &endpoint, TimePoint now)
{
	std::lock_guard lock(mMutex);

	auto			it = findIt(endpoint);

	if (it == mKnownPeers.end())
		return false;

	it->lastSeen = now;
	return true;
}


bool DiscoveryRegistry::remove(const DiscoveryEndpoint &endpoint)
{
	std::lock_guard lock(mMutex);

	auto			it = findIt(endpoint);

	if (it == mKnownPeers.end())
		return false;

	mKnownPeers.erase(it);
	return true;
}


std::vector<DiscoveryEndpoint> DiscoveryRegistry::removeStale(std::chrono::milliseconds timeout, TimePoint now)
{
	std::lock_guard				   lock(mMutex);

	std::vector<DiscoveryEndpoint> removed;

	auto						   staleBegin = std::remove_if(mKnownPeers.begin(), mKnownPeers.end(),
															   [&](const KnownPeer &peer)
															   {
										 bool isStale = (now - peer.lastSeen) > timeout;
										 
										 if (isStale)
											 removed.push_back(peer.endpoint);

										 return isStale;
															   });

	mKnownPeers.erase(staleBegin, mKnownPeers.end());
	return removed;
}


bool DiscoveryRegistry::contains(const DiscoveryEndpoint &endpoint) const
{
	std::lock_guard lock(mMutex);
	return findIt(endpoint) != mKnownPeers.end();
}


std::optional<DiscoveryRegistry::KnownPeer> DiscoveryRegistry::find(const DiscoveryEndpoint &endpoint) const
{
	std::lock_guard lock(mMutex);

	auto			it = findIt(endpoint);

	if (it == mKnownPeers.end())
		return std::nullopt;

	return *it;
}


std::vector<DiscoveryRegistry::KnownPeer> DiscoveryRegistry::snapshot() const
{
	std::lock_guard lock(mMutex);
	return mKnownPeers;
}


size_t DiscoveryRegistry::size() const
{
	std::lock_guard lock(mMutex);
	return mKnownPeers.size();
}


void DiscoveryRegistry::clear()
{
	std::lock_guard lock(mMutex);
	mKnownPeers.clear();
}

} // namespace netlink::discovery
