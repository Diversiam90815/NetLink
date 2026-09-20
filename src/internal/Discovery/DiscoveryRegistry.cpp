/*
  ==============================================================================
	Module:         DiscoveryRegistry
	Description:    Bookkeeping of remote known peers
  ==============================================================================
*/

#include "DiscoveryRegistry.h"


namespace netlink::discovery
{

auto DiscoveryRegistry::findIt(const netlink::net::IPv4Address &ip)
{
	return std::ranges::find_if(mKnownPeers, [&](const KnownPeer &p) { return p.endpoint.IPAddress == ip; });
}


auto DiscoveryRegistry::findIt(const netlink::net::IPv4Address &ip) const
{
	return std::ranges::find_if(mKnownPeers, [&](const KnownPeer &p) { return p.endpoint.IPAddress == ip; });
}


DiscoveryRegistry::UpdateResult DiscoveryRegistry::addOrUpdate(const DiscoveryEndpoint &endpoint, TimePoint now)
{
	std::lock_guard lock(mMutex);

	auto			it = findIt(endpoint.IPAddress);

	if (it != mKnownPeers.end())
	{
		bool metadataChanged = !(it->endpoint.port == endpoint.port && it->endpoint.displayName == endpoint.displayName);

		it->endpoint		 = endpoint;
		it->lastSeen		 = now;

		return metadataChanged ? UpdateResult::Updated : UpdateResult::Refreshed;
	}

	mKnownPeers.push_back(KnownPeer{endpoint, now});
	return UpdateResult::Added;
}


bool DiscoveryRegistry::touch(const DiscoveryEndpoint &endpoint, TimePoint now)
{
	std::lock_guard lock(mMutex);

	auto			it = findIt(endpoint.IPAddress);

	if (it == mKnownPeers.end())
		return false;

	it->lastSeen = now;
	return true;
}


bool DiscoveryRegistry::remove(const DiscoveryEndpoint &endpoint)
{
	std::lock_guard lock(mMutex);

	auto			it = findIt(endpoint.IPAddress);

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
	return findIt(endpoint.IPAddress) != mKnownPeers.end();
}


std::optional<DiscoveryRegistry::KnownPeer> DiscoveryRegistry::find(const DiscoveryEndpoint &endpoint) const
{
	std::lock_guard lock(mMutex);

	auto			it = findIt(endpoint.IPAddress);

	if (it == mKnownPeers.end())
		return std::nullopt;

	return *it;
}


std::optional<DiscoveryRegistry::KnownPeer> DiscoveryRegistry::findByIP(const netlink::net::IPv4Address &ip) const
{
	std::lock_guard lock(mMutex);

	auto			it = std::ranges::find_if(mKnownPeers, [&](const KnownPeer &e) { return e.endpoint.IPAddress == ip; });

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
