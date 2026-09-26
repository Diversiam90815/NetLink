/*
  ==============================================================================
	Module:         HeartbeatService
	Description:    Liveness of watched peers over the connectionless channel
  ==============================================================================
*/

#pragma once

#include <chrono>
#include <map>
#include <optional>
#include <vector>

#include "Socket/SocketTypes.h"


namespace netlink::channel
{

struct HeartbeatConfig
{
	std::chrono::milliseconds interval{1000};		// idle time before a heartbeat is sent
	std::chrono::milliseconds silenceTimeout{5000}; // no inbound traffic for this long -> peer lost
};


struct HeartbeatTick
{
	std::vector<net::SocketAddress> heartbeatsDue; // send a heartbeat to each
	std::vector<net::SocketAddress> silentPeers;   // considered lost, no longer watched
};


class HeartbeatService
{
public:
	using Clock		= std::chrono::steady_clock;
	using TimePoint = Clock::time_point;

	explicit HeartbeatService(const HeartbeatConfig config = {}) : mConfig(config) {}

	void					 setConfig(const HeartbeatConfig &config) { mConfig = config; }
	const HeartbeatConfig	&config() const { return mConfig; }

	// Starts supervising a peer; it counts as alive right now
	void					 watch(const net::SocketAddress &peer, TimePoint now);
	void					 unwatch(const net::SocketAddress &peer);
	bool					 isWatched(const net::SocketAddress &peer) const;

	// Any traffic counts: data, acks and heartbeats. Ignored for peers that are not watched.
	void					 onSent(const net::SocketAddress &peer, TimePoint now);
	void					 onReceived(const net::SocketAddress &peer, TimePoint now);

	// Silent peers are reported once and stop being watched
	HeartbeatTick			 tick(TimePoint now);

	std::optional<TimePoint> nextDeadline() const;

	void					 clear() { mPeers.clear(); }

private:
	struct PeerState
	{
		TimePoint lastSent;
		TimePoint lastReceived;
	};

	HeartbeatConfig							mConfig;
	std::map<net::SocketAddress, PeerState> mPeers;
};

} // namespace netlink::channel
