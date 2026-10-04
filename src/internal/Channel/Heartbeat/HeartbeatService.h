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
#include "TransportConstants.h"


namespace netlink::channel
{

struct HeartbeatTick
{
	std::vector<net::SocketAddress> pingsDue;	 // ask each of them for a sign of life
	std::vector<net::SocketAddress> silentPeers; // considered lost, no longer watched
};


// A peer that was not heard from for a while is asked (it answers every Ping), and given up on when it stays silent
class HeartbeatService
{
public:
	using Clock		= std::chrono::steady_clock;
	using TimePoint = Clock::time_point;

	explicit HeartbeatService(const LinkTimings &timings = {}) { setTimings(timings); }

	void setTimings(const LinkTimings &timings)
	{
		mInterval		= timings.keepAlive;
		mSilenceTimeout = timings.peerTimeout;
	}

	// Starts supervising a peer; it counts as alive right now
	void					 watch(const net::SocketAddress &peer, TimePoint now);
	void					 unwatch(const net::SocketAddress &peer);
	bool					 isWatched(const net::SocketAddress &peer) const;

	// Any traffic counts: data, acknowledgements and pings. Ignored for peers that are not watched.
	void					 onReceived(const net::SocketAddress &peer, TimePoint now);

	// Silent peers are reported once and stop being watched
	HeartbeatTick			 tick(TimePoint now);

	std::optional<TimePoint> nextDeadline() const;

	void					 clear() { mPeers.clear(); }

private:
	struct PeerState
	{
		TimePoint lastReceived;
		TimePoint lastAsked; // the latest Ping, or what was received after it

		TimePoint askAt(const std::chrono::milliseconds interval) const { return std::max(lastReceived, lastAsked) + interval; }
	};

	std::chrono::milliseconds				mInterval{};
	std::chrono::milliseconds				mSilenceTimeout{};
	std::map<net::SocketAddress, PeerState> mPeers;
};

} // namespace netlink::channel
