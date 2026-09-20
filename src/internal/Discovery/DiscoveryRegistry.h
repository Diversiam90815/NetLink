/*
  ==============================================================================
	Module:         DiscoveryRegistry
	Description:    Bookkeeping of remote known peers
  ==============================================================================
*/


#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <vector>
#include <algorithm>

#include "DiscoveryEndpoint.h"


namespace netlink::discovery
{

class DiscoveryRegistry
{
public:
	using TimePoint = std::chrono::steady_clock::time_point;

	struct KnownPeer
	{
		DiscoveryEndpoint endpoint;
		TimePoint		  lastSeen;
	};

	enum class UpdateResult
	{
		Added,	 // New peer, not previously known
		Updated, // Existing peer, timestamp (and/or metadata) refreshed
	};

	DiscoveryRegistry()													= default;
	~DiscoveryRegistry()												= default;
	DiscoveryRegistry(const DiscoveryRegistry &)						= delete;
	DiscoveryRegistry			  &operator=(const DiscoveryRegistry &) = delete;

	UpdateResult				   addOrUpdate(const DiscoveryEndpoint &endpoint, TimePoint now = std::chrono::steady_clock::now());

	// Explicitly refreshes the lastSeen timestamp for an existing peer (no-op if not found).
	// Returns true if the peer existed and was touched.
	bool						   touch(const DiscoveryEndpoint &endpoint, TimePoint now = std::chrono::steady_clock::now());

	// Removes a peer explicitly. Returns true if it existed.
	bool						   remove(const DiscoveryEndpoint &endpoint);

	// Removes all peers whose lastSeen is older than 'timeout' relative to 'now' & returns all removed endpoints
	std::vector<DiscoveryEndpoint> removeStale(std::chrono::milliseconds timeout, TimePoint now = std::chrono::steady_clock::now());

	bool						   contains(const DiscoveryEndpoint &endpoint) const;
	std::optional<KnownPeer>	   find(const DiscoveryEndpoint &endpoint) const;
	std::optional<KnownPeer>	   findByIP(const netlink::net::IPv4Address &ip) const;
	std::vector<KnownPeer>		   snapshot() const;
	size_t						   size() const;
	void						   clear();

private:
	mutable std::mutex	   mMutex;
	std::vector<KnownPeer> mKnownPeers;

	// Non-locking helpers, must be called while holding mMutex
	auto				   findIt(const DiscoveryEndpoint &endpoint);
	auto				   findIt(const DiscoveryEndpoint &endpoint) const;
};

} // namespace netlink::discovery