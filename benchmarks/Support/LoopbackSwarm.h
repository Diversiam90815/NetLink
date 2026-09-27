/*
  ==============================================================================
	Module:         LoopbackSwarm
	Description:    One hub PeerChannel and many peer PeerChannels, all
					production channels on real UDP sockets bound to 127.0.0.1.
  ==============================================================================
*/

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "BenchUtil.h"
#include "Channel/PeerChannel.h"


namespace bench
{

class LoopbackSwarm
{
public:
	static constexpr auto HubName = "hub";

	explicit LoopbackSwarm(const size_t peerCount, const netlink::PeerChannelConfig &config = {}) : hub({}, config)
	{
		for (size_t i = 0; i < peerCount; ++i)
		{
			peers.push_back(std::make_unique<netlink::PeerChannel>(netlink::net::DatagramSocketFactory{}, config));
			names.push_back("peer-" + std::to_string(i));
		}
	}

	~LoopbackSwarm()
	{
		hub.deinit();
		for (const auto &peer : peers)
			peer->deinit();
	}

	LoopbackSwarm(const LoopbackSwarm &)			= delete;
	LoopbackSwarm &operator=(const LoopbackSwarm &) = delete;

	// Binds every channel to loopback and registers hub <-> peers. False if a socket could not be bound.
	bool		   open()
	{
		if (!hub.init(HubName))
			return false;

		hub.setLocalIPv4(loopback());
		if (hub.getBoundPort() == 0)
			return false;

		for (size_t i = 0; i < peers.size(); ++i)
		{
			auto &peer = *peers[i];

			if (!peer.init(names[i]))
				return false;

			peer.setLocalIPv4(loopback());
			if (peer.getBoundPort() == 0)
				return false;

			peer.registerPeer(HubName, loopback(), hub.getBoundPort());
			hub.registerPeer(names[i], loopback(), peer.getBoundPort());
		}

		return true;
	}

	// Starts every I/O loop. Callbacks must be set before.
	void start()
	{
		hub.start();
		for (const auto &peer : peers)
			peer->start();
	}

	// Stops every I/O loop, so no channel thread calls into anything anymore
	void stop()
	{
		hub.stop();
		for (const auto &peer : peers)
			peer->stop();
	}

	size_t											   size() const { return peers.size(); }

	netlink::PeerChannel							   hub;
	std::vector<std::unique_ptr<netlink::PeerChannel>> peers;
	std::vector<std::string>						   names; // names[i] belongs to peers[i]
};

} // namespace bench
