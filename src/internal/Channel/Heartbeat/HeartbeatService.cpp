/*
  ==============================================================================
	Module:         HeartbeatService
	Description:    Liveness of watched peers over the connectionless channel
  ==============================================================================
*/

#include "HeartbeatService.h"

#include <algorithm>
#include <ranges>


namespace netlink::channel
{

void HeartbeatService::watch(const net::SocketAddress &peer, const TimePoint now)
{
	mPeers[peer] = {now, now};
}


void HeartbeatService::unwatch(const net::SocketAddress &peer)
{
	mPeers.erase(peer);
}


bool HeartbeatService::isWatched(const net::SocketAddress &peer) const
{
	return mPeers.contains(peer);
}


void HeartbeatService::onSent(const net::SocketAddress &peer, const TimePoint now)
{
	if (const auto it = mPeers.find(peer); it != mPeers.end())
		it->second.lastSent = std::max(it->second.lastSent, now);
}


void HeartbeatService::onReceived(const net::SocketAddress &peer, const TimePoint now)
{
	if (const auto it = mPeers.find(peer); it != mPeers.end())
		it->second.lastReceived = std::max(it->second.lastReceived, now);
}


HeartbeatTick HeartbeatService::tick(const TimePoint now)
{
	HeartbeatTick result;

	for (auto it = mPeers.begin(); it != mPeers.end();)
	{
		if (now - it->second.lastReceived >= mConfig.silenceTimeout)
		{
			result.silentPeers.push_back(it->first);
			it = mPeers.erase(it);
			continue;
		}

		if (now - it->second.lastSent >= mConfig.interval)
		{
			result.heartbeatsDue.push_back(it->first);
			it->second.lastSent = now;
		}

		++it;
	}

	return result;
}


std::optional<HeartbeatService::TimePoint> HeartbeatService::nextDeadline() const
{
	std::optional<TimePoint> next;

	for (const auto &[lastSent, lastReceived] : mPeers | std::views::values)
	{
		if (const TimePoint due = std::min(lastSent + mConfig.interval, lastReceived + mConfig.silenceTimeout);!next || due < *next)
			next = due;
	}

	return next;
}

} // namespace netlink::channel
