/*
  ==============================================================================
	Module:         Loopback
	Description:    Production PeerChannels on real UDP sockets bound to
					127.0.0.1, and application-like sending over them
  ==============================================================================
*/

#pragma once

#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "BenchUtil.h"
#include "Channel/PeerChannel.h"


namespace bench
{

// One hub and N peers. Every peer knows the hub and the hub knows every peer; a plain pair is LoopbackPeers(1).
class LoopbackPeers
{
public:
	static constexpr auto HubName = "hub";

	explicit LoopbackPeers(const size_t peerCount, const netlink::PeerChannelConfig &config = {}) : mHub({}, config)
	{
		for (size_t i = 0; i < peerCount; ++i)
		{
			mPeers.push_back(std::make_unique<netlink::PeerChannel>(netlink::net::DatagramSocketFactory{}, config));
			mNames.push_back("peer-" + std::to_string(i));
		}
	}

	~LoopbackPeers()
	{
		mHub.deinit();
		for (const auto &peer : mPeers)
			peer->deinit();
	}

	LoopbackPeers(const LoopbackPeers &)			= delete;
	LoopbackPeers &operator=(const LoopbackPeers &) = delete;

	// Binds every channel to loopback and registers hub <-> peers. False if a socket could not be bound.
	bool		   open()
	{
		if (!bind(mHub, HubName))
			return false;

		for (size_t i = 0; i < mPeers.size(); ++i)
		{
			if (!bind(*mPeers[i], mNames[i]))
				return false;

			mPeers[i]->registerPeer(HubName, loopback(), mHub.getBoundPort());
			mHub.registerPeer(mNames[i], loopback(), mPeers[i]->getBoundPort());
		}

		return true;
	}

	// Starts every I/O loop. Callbacks must be set before.
	void start()
	{
		mHub.start();
		for (const auto &peer : mPeers)
			peer->start();
	}

	// Stops every I/O loop, so no channel thread calls into anything anymore
	void stop()
	{
		mHub.stop();
		for (const auto &peer : mPeers)
			peer->stop();
	}

	netlink::PeerChannel &hub() { return mHub; }
	netlink::PeerChannel &peer(const size_t i) { return *mPeers[i]; }
	const std::string	 &peerName(const size_t i) const { return mNames[i]; }
	size_t				  peerCount() const { return mPeers.size(); }

private:
	static bool bind(netlink::PeerChannel &channel, const std::string &name)
	{
		if (!channel.init(name))
			return false;

		channel.setLocalIPv4(loopback());
		return channel.getBoundPort() != 0;
	}

	netlink::PeerChannel							   mHub;
	std::vector<std::unique_ptr<netlink::PeerChannel>> mPeers;
	std::vector<std::string>						   mNames; // mNames[i] belongs to mPeers[i]
};


// Sends like an application that respects backpressure: while the send queue is full, the send waits for room.
// False if the message could not be queued within the timeout.
inline bool sendWithBackpressure(netlink::PeerChannel &channel, const std::string &to, const uint32_t type, std::span<const uint8_t> payload,
								 const std::chrono::milliseconds timeout = WaitTimeout)
{
	return channel.sendMessage(to, type, payload, netlink::DeliveryMode::ReliableOrdered, timeout);
}


// Streams 1 KiB reliable messages to one peer for as long as it lives, like an application saturating the channel
class BackgroundStream
{
public:
	BackgroundStream(netlink::PeerChannel &channel, std::string to) : mChannel(channel), mTo(std::move(to)), mThread([this] { run(); }) {}

	~BackgroundStream()
	{
		mStop.store(true);
		mThread.join();
	}

	BackgroundStream(const BackgroundStream &)			  = delete;
	BackgroundStream &operator=(const BackgroundStream &) = delete;

private:
	void run()
	{
		while (!mStop.load())
			sendWithBackpressure(mChannel, mTo, 1, mPayload, std::chrono::milliseconds{100});
	}

	netlink::PeerChannel	  &mChannel;
	std::string				   mTo;
	const std::vector<uint8_t> mPayload = makePayload(KiB);
	std::atomic<bool>		   mStop{false};
	std::thread				   mThread; // declared last: starts once everything it uses exists
};

} // namespace bench
