/*
  ==============================================================================
	Module:         EngineHarness
	Description:    What tests of the engine share: a record of its events, the
					engine on threads of its own, and a socket whose cable can
					be pulled
  ==============================================================================
*/

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "Engine/NetworkEngine.h"
#include "FakeDatagramNetwork.h"
#include "TaskQueue.h"
#include "TestIp.h"


namespace FakeNet
{

struct ReceivedMessage
{
	netlink::PeerId		 from;
	netlink::Lane		 lane{netlink::Lane::Reliable};
	uint32_t			 type{0};
	std::vector<uint8_t> data;
};

struct Ended
{
	netlink::PeerId			  peer;
	netlink::DisconnectReason reason{netlink::DisconnectReason::Local};

	bool					  operator==(const Ended &) const = default;
};


// Everything an engine reported, in the order it was reported. Safe to read while the engine runs.
class EventRecorder
{
public:
	void record(netlink::EngineEvent &event)
	{
		using Kind = netlink::EngineEvent::Kind;

		std::unique_lock<std::mutex> lock(mMutex);

		if (event.kind != Kind::Message)
			mOrder.emplace_back(event.kind, event.peer);

		switch (event.kind)
		{
		case Kind::PeerDiscovered: mDiscovered.push_back(event.info); break;
		case Kind::PeerLost: mLost.push_back(event.peer); break;
		case Kind::ConnectionRequest: mRequests.push_back(event.info); break;
		case Kind::Connected: mConnected.push_back(event.info); break;
		case Kind::Disconnected: mEnded.push_back({event.peer, event.reason}); break;
		case Kind::AdapterChanged: mAddresses.push_back(event.info.address); break;

		case Kind::Message:
		{
			mCallbackThread = std::this_thread::get_id();
			++mEnteredCallbacks;
			mReleased.wait(lock, [this] { return !mHolding; });

			if (auto message = event.takeMessage())
			{
				mOrder.emplace_back(event.kind, event.peer);
				mMessages.push_back({event.peer, event.lane, message->type, std::move(message->data)});
			}
			break;
		}
		}
	}

	// Message callbacks block until releaseMessages(), like an application that is busy in its handler
	void holdMessages()
	{
		std::lock_guard<std::mutex> lock(mMutex);
		mHolding = true;
	}

	void releaseMessages()
	{
		{
			std::lock_guard<std::mutex> lock(mMutex);
			mHolding = false;
		}
		mReleased.notify_all();
	}

	std::vector<netlink::PeerInfo>										discovered() const { return copyOf(mDiscovered); }
	std::vector<netlink::PeerId>										lost() const { return copyOf(mLost); }
	std::vector<netlink::PeerInfo>										requests() const { return copyOf(mRequests); }
	std::vector<netlink::PeerInfo>										connected() const { return copyOf(mConnected); }
	std::vector<Ended>													ended() const { return copyOf(mEnded); }
	std::vector<std::string>											addresses() const { return copyOf(mAddresses); }
	std::vector<ReceivedMessage>										messages() const { return copyOf(mMessages); }
	std::vector<std::pair<netlink::EngineEvent::Kind, netlink::PeerId>> order() const { return copyOf(mOrder); }

	size_t																messageCount() const
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return mMessages.size();
	}

	int enteredMessageCallbacks() const
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return mEnteredCallbacks;
	}

	std::thread::id messageThread() const
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return mCallbackThread;
	}

	bool knows(const netlink::PeerId peer) const
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return std::ranges::any_of(mDiscovered, [&](const netlink::PeerInfo &info) { return info.id == peer; });
	}

	bool isConnectedTo(const netlink::PeerId peer) const
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return std::ranges::any_of(mConnected, [&](const netlink::PeerInfo &info) { return info.id == peer; });
	}

private:
	template <typename T>
	T copyOf(const T &value) const
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return value;
	}

	mutable std::mutex													mMutex;
	std::condition_variable												mReleased;
	bool																mHolding{false};
	int																	mEnteredCallbacks{0};
	std::thread::id														mCallbackThread;
	std::vector<netlink::PeerInfo>										mDiscovered;
	std::vector<netlink::PeerId>										mLost;
	std::vector<netlink::PeerInfo>										mRequests;
	std::vector<netlink::PeerInfo>										mConnected;
	std::vector<Ended>													mEnded;
	std::vector<std::string>											mAddresses;
	std::vector<ReceivedMessage>										mMessages;
	std::vector<std::pair<netlink::EngineEvent::Kind, netlink::PeerId>> mOrder;
};


// The address an engine is given. A test can change it, like an adapter that gets a new address or loses it.
class TestInterface
{
public:
	explicit TestInterface(const std::string_view ip) { set(ip); }

	void set(const std::string_view ip, const std::string_view mask = "255.255.255.0")
	{
		std::lock_guard<std::mutex> lock(mMutex);
		mCurrent = netlink::LocalInterface{.ip = ipv4(ip), .mask = ipv4(mask)};
	}

	void clear()
	{
		std::lock_guard<std::mutex> lock(mMutex);
		mCurrent.reset();
	}

	netlink::LocalInterfaceProvider provider()
	{
		return [this]
		{
			std::lock_guard<std::mutex> lock(mMutex);
			return mCurrent;
		};
	}

private:
	std::mutex							   mMutex;
	std::optional<netlink::LocalInterface> mCurrent;
};


// An engine with its I/O thread and its event thread, as the library runs it
class ThreadedEngine
{
public:
	ThreadedEngine(netlink::EngineConfig config, DatagramSocketFactory factory, const std::string_view ip)
		: iface(ip), engine(std::move(config), std::move(factory), iface.provider())
	{
	}

	~ThreadedEngine() { stop(); }

	void start()
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
										   events.record(event);
								   });
						   });
			});
	}

	// Ends the threads without a word to anyone, like a process that is killed
	void stop()
	{
		events.releaseMessages();

		if (mThread.joinable())
		{
			mThread.request_stop();
			mThread.join();
		}

		mEvents.stop();
	}

	netlink::PeerId		   id() const { return engine.id(); }

	EventRecorder		   events;
	TestInterface		   iface;
	netlink::NetworkEngine engine;

private:
	TaskQueue	 mEvents;
	std::jthread mThread;
};


// Cutting a socket drops everything it sends and receives, as if its cable was pulled
class CuttableSocket final : public IDatagramSocket
{
public:
	CuttableSocket(std::unique_ptr<IDatagramSocket> inner, std::shared_ptr<std::atomic<bool>> cut) : mInner(std::move(inner)), mCut(std::move(cut)) {}

	static DatagramSocketFactory wrap(DatagramSocketFactory inner, std::shared_ptr<std::atomic<bool>> cut)
	{
		return [inner = std::move(inner), cut = std::move(cut)](const SocketAddress &local, const BindOptions &options) -> Result<std::unique_ptr<IDatagramSocket>>
		{
			auto socket = inner(local, options);
			if (!socket)
				return std::unexpected(socket.error());

			return std::make_unique<CuttableSocket>(std::move(*socket), cut);
		};
	}

	Result<size_t> sendTo(const SocketAddress &destination, std::span<const uint8_t> data) override
	{
		return mCut->load() ? Result<size_t>(data.size()) : mInner->sendTo(destination, data);
	}

	Result<Datagram> receiveFrom(std::span<uint8_t> buffer, std::chrono::microseconds timeout) override
	{
		auto datagram = mInner->receiveFrom(buffer, timeout);
		if (datagram && mCut->load())
			return std::unexpected(SocketError::Timeout);
		return datagram;
	}

	Result<void>  waitReadable(std::chrono::microseconds timeout) override { return mInner->waitReadable(timeout); }
	void		  interrupt() override { mInner->interrupt(); }
	SocketAddress localAddress() const override { return mInner->localAddress(); }
	void		  shutdown() override { mInner->shutdown(); }

private:
	std::unique_ptr<IDatagramSocket>   mInner;
	std::shared_ptr<std::atomic<bool>> mCut;
};


template <typename Predicate>
bool waitUntilTrue(Predicate predicate, const std::chrono::milliseconds timeout = std::chrono::seconds{3})
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;

	while (std::chrono::steady_clock::now() < deadline)
	{
		if (predicate())
			return true;

		std::this_thread::sleep_for(std::chrono::milliseconds{2});
	}

	return predicate();
}


// Both announce themselves until each knows the other. Announcing again wakes an engine, which then also looks at
// what arrived on its discovery port: otherwise it only does that every two seconds.
inline bool discover(ThreadedEngine &a, ThreadedEngine &b, const std::chrono::milliseconds timeout = std::chrono::seconds{5})
{
	return waitUntilTrue(
		[&]
		{
			a.engine.setAnnouncing(true);
			b.engine.setAnnouncing(true);
			return a.events.knows(b.id()) && b.events.knows(a.id());
		},
		timeout);
}


// a asks b for a session and both report it
inline bool connect(ThreadedEngine &a, ThreadedEngine &b, const std::chrono::milliseconds timeout = std::chrono::seconds{5})
{
	if (!discover(a, b, timeout) || !a.engine.connect(b.id()))
		return false;

	return waitUntilTrue([&] { return a.events.isConnectedTo(b.id()) && b.events.isConnectedTo(a.id()); }, timeout);
}

} // namespace FakeNet
