/*
  ==============================================================================
	Module:         LoopbackChannelPair
	Description:    Two production PeerChannels on real UDP sockets bound to
					127.0.0.1, registered with each other
  ==============================================================================
*/

#pragma once

#include "BenchUtil.h"
#include "Channel/PeerChannel.h"


namespace bench
{

class LoopbackChannelPair
{
public:
	static constexpr auto NameA = "bench-a";
	static constexpr auto NameB = "bench-b";

	explicit LoopbackChannelPair(const netlink::PeerChannelConfig &config = {}) : a({}, config), b({}, config) {}

	~LoopbackChannelPair()
	{
		a.deinit();
		b.deinit();
	}

	LoopbackChannelPair(const LoopbackChannelPair &)			= delete;
	LoopbackChannelPair &operator=(const LoopbackChannelPair &) = delete;

	// Binds both channels to loopback and makes them known to each other. False if a socket could not be bound.
	bool				 open()
	{
		if (!a.init(NameA) || !b.init(NameB))
			return false;

		a.setLocalIPv4(loopback());
		b.setLocalIPv4(loopback());

		if (a.getBoundPort() == 0 || b.getBoundPort() == 0)
			return false;

		a.registerPeer(NameB, loopback(), b.getBoundPort());
		b.registerPeer(NameA, loopback(), a.getBoundPort());
		return true;
	}

	// Starts both I/O loops. Callbacks must be set before.
	void start()
	{
		a.start();
		b.start();
	}

	netlink::PeerChannel a;
	netlink::PeerChannel b;
};

} // namespace bench
