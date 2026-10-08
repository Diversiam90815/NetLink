/*
  ==============================================================================
	Module:         Loopback
	Description:    Production engines on real UDP sockets bound to 127.0.0.1,
					with the two threads the library runs them on
  ==============================================================================
*/

#pragma once

#include <algorithm>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "BenchUtil.h"
#include "Engine/NetworkEngine.h"
#include "TaskQueue.h"


namespace bench
{

// Kept off NetLink's default port, so an application running on this machine is not disturbed
inline constexpr uint16_t	 DiscoveryPort = 45455;

// What the default configuration allows
inline netlink::EngineConfig defaultRate()
{
	netlink::EngineConfig config;
	config.appId		 = "netlink-benchmarks";
	config.appVersion	 = "1.4.2";
	config.discoveryPort = DiscoveryPort;
	return config;
}

// An engine without a send budget: what the code and the machine can do, not what the default rate allows
inline netlink::EngineConfig unlimitedRate()
{
	netlink::EngineConfig config = defaultRate();
	config.maxSendRate			 = 0;
	return config;
}


// An engine on a loopback address. Its events are delivered on an event thread, like NetLinkCore does it.
class EngineHost
{
public:
	using MessageHandler = std::function<void(netlink::PeerId from, uint32_t type, std::vector<uint8_t> &data)>;

	EngineHost(netlink::EngineConfig config, const std::string &name, const netlink::net::IPv4Address ip = loopback())
		: engine(named(std::move(config), name), {},
				 [ip] { return std::optional(netlink::LocalInterface{.ip = ip, .mask = netlink::net::IPv4Address::fromHostOrder(0xFF000000u)}); })
	{
	}

	~EngineHost() { stop(); }

	EngineHost(const EngineHost &)			  = delete;
	EngineHost &operator=(const EngineHost &) = delete;

	// Set before start(). Called on the event thread.
	void		setMessageHandler(MessageHandler handler) { mOnMessage = std::move(handler); }
	void		setLossHandler(std::function<void()> handler) { mOnLoss = std::move(handler); }

	void		start()
	{
		mEvents.start();
		mThread = std::jthread(
			[this](const std::stop_token &stop)
			{
				engine.run(stop,
						   [this](netlink::EventBatch &&batch)
						   {
							   mEvents.post(
								   [this, shared = std::make_shared<netlink::EventBatch>(std::move(batch))]
								   {
									   for (auto &event : shared->events)
										   handle(event);
								   });
						   });
			});
	}

	// Stops both threads, so nothing calls into the benchmark anymore
	void stop()
	{
		if (mThread.joinable())
		{
			mThread.request_stop();
			mThread.join();
		}

		mEvents.stop();
	}

	netlink::PeerId id() const { return engine.id(); }

	bool			knows(const netlink::PeerId peer)
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return std::ranges::find(mDiscovered, peer) != mDiscovered.end();
	}

	// Waits until a session with that peer was reported
	bool waitForSession(const netlink::PeerId peer, const std::chrono::milliseconds timeout = WaitTimeout)
	{
		std::unique_lock<std::mutex> lock(mMutex);
		return mChanged.wait_for(lock, timeout, [&] { return std::ranges::find(mConnected, peer) != mConnected.end(); });
	}

	// ... or that it ended
	bool waitForEnd(const netlink::PeerId peer, const std::chrono::milliseconds timeout = WaitTimeout)
	{
		std::unique_lock<std::mutex> lock(mMutex);
		return mChanged.wait_for(lock, timeout, [&] { return std::ranges::find(mConnected, peer) == mConnected.end(); });
	}

	netlink::NetworkEngine engine;

private:
	static netlink::EngineConfig named(netlink::EngineConfig config, const std::string &name)
	{
		config.displayName = name;
		return config;
	}

	void handle(netlink::EngineEvent &event)
	{
		using Kind = netlink::EngineEvent::Kind;

		switch (event.kind)
		{
		case Kind::Message:
			if (auto message = event.takeMessage(); message && mOnMessage)
				mOnMessage(event.peer, message->type, message->data);
			return;

		case Kind::PeerDiscovered:
		{
			std::lock_guard<std::mutex> lock(mMutex);
			mDiscovered.push_back(event.peer);
			break;
		}

		case Kind::Connected:
		{
			std::lock_guard<std::mutex> lock(mMutex);
			mConnected.push_back(event.peer);
			break;
		}

		case Kind::Disconnected:
		{
			{
				std::lock_guard<std::mutex> lock(mMutex);
				std::erase(mConnected, event.peer);
			}

			if (mOnLoss && event.reason != netlink::DisconnectReason::Local && event.reason != netlink::DisconnectReason::Remote)
				mOnLoss();
			break;
		}

		default: return;
		}

		mChanged.notify_all();
	}

	MessageHandler				 mOnMessage;
	std::function<void()>		 mOnLoss;

	std::mutex					 mMutex;
	std::condition_variable		 mChanged;
	std::vector<netlink::PeerId> mDiscovered;
	std::vector<netlink::PeerId> mConnected;

	TaskQueue					 mEvents;
	std::jthread				 mThread; // declared last: starts once everything it uses exists
};


// Both announce themselves until each found the other. Announcing again makes an engine look at its discovery port
// right away instead of with its next tick.
inline bool discover(EngineHost &a, EngineHost &b, const std::chrono::milliseconds timeout = WaitTimeout)
{
	const auto deadline = Clock::now() + timeout;

	while (Clock::now() < deadline)
	{
		a.engine.setAnnouncing(true);
		b.engine.setAnnouncing(true);

		if (a.knows(b.id()) && b.knows(a.id()))
			return true;

		std::this_thread::sleep_for(std::chrono::milliseconds{2});
	}

	return false;
}

// from asks to for a session; true once both report it
inline bool connect(EngineHost &from, EngineHost &to)
{
	return discover(from, to) && from.engine.connect(to.id()) && from.waitForSession(to.id()) && to.waitForSession(from.id());
}


// One hub and N peers, each peer with a session to the hub; a plain pair is LoopbackPeers(1)
class LoopbackPeers
{
public:
	explicit LoopbackPeers(const size_t peerCount, const netlink::EngineConfig &config = defaultRate()) : mHub(config, "hub")
	{
		for (size_t i = 0; i < peerCount; ++i)
			mPeers.push_back(std::make_unique<EngineHost>(config, "peer-" + std::to_string(i)));
	}

	~LoopbackPeers() { stop(); }

	LoopbackPeers(const LoopbackPeers &)			= delete;
	LoopbackPeers &operator=(const LoopbackPeers &) = delete;

	// Starts every engine and connects the peers to the hub. Handlers must be set before. False if that did not work.
	bool		   open()
	{
		mHub.start();
		for (const auto &peer : mPeers)
			peer->start();

		for (const auto &peer : mPeers)
		{
			if (!connect(*peer, mHub))
				return false;
		}

		// Announcements are of no use anymore, and would only add to what is measured
		mHub.engine.setAnnouncing(false);
		for (const auto &peer : mPeers)
			peer->engine.setAnnouncing(false);

		return true;
	}

	// Stops every engine, so no thread calls into anything anymore
	void stop()
	{
		mHub.stop();
		for (const auto &peer : mPeers)
			peer->stop();
	}

	EngineHost &hub() { return mHub; }
	EngineHost &peer(const size_t i) { return *mPeers[i]; }
	size_t		peerCount() const { return mPeers.size(); }

private:
	EngineHost								 mHub;
	std::vector<std::unique_ptr<EngineHost>> mPeers;
};


// Sends like an application that respects backpressure: while the send queue is full, the send waits for room.
// False if the message could not be queued within the timeout.
inline bool sendWithBackpressure(EngineHost &from, const netlink::PeerId to, const uint32_t type, const std::span<const uint8_t> payload,
								 const std::chrono::milliseconds timeout = WaitTimeout)
{
	return from.engine.send(to, type, std::vector<uint8_t>(payload.begin(), payload.end()), netlink::Lane::Reliable, timeout) == netlink::SendResult::Queued;
}

} // namespace bench
