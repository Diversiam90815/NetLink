/*
  ==============================================================================
	Module:         HandshakeTracker
	Description:    Thread-safe tracker for the two-way "handshake" exchange
  ==============================================================================
*/

#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "Discovery/DiscoveryEndpoint.h"


namespace netlink
{

struct RemoteHandshake
{
	std::string							  remoteName{};
	DiscoveryEndpoint					  remoteEndpoint{};
	bool								  sent{false};
	bool								  received{false};
	std::chrono::steady_clock::time_point initiatedTime;

	bool								  isComplete() const { return sent && received; }
};


class HandshakeTracker
{
public:
	// Call when we (locally) discover a peer. Returns true if a new handshake entry was created.
	bool							 beginForDiscoveredPeer(const DiscoveryEndpoint &endpoint);

	// Call when a handshake is received from the remote. Returns true if a new handshake
	// entry was created (i.e. remote reached us before we discovered them).
	bool							 markReceived(const std::string &computerName);

	// Call after we've sent our handshake to the remote.
	void							 markSent(const std::string &computerName);

	// Returns the remote endpoint and removes the entry ONLY if the handshake is fully
	// complete (both sent and received). Returns std::nullopt otherwise (still waiting).
	std::optional<DiscoveryEndpoint> tryCompleteAndRemove(const std::string &computerName);

	void							 remove(const std::string &computerName);

	// Diagnostic helper
	std::optional<RemoteHandshake>	 get(const std::string &computerName) const;

private:
	std::map<std::string, RemoteHandshake> mHandshakes; // key = computerName
	mutable std::mutex					   mMutex;
};

} // namespace netlink
