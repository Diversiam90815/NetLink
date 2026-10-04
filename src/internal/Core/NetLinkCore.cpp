/*
  ==============================================================================
	Module:         NetLinkCore
	Description:    Runs the engine on its thread, delivers its events on the
					event thread and tells it which network adapter to use
  ==============================================================================
*/

#include "NetLinkCore.h"

#include <algorithm>

#include "NetLinkLog.h"
#include "NetLinkVersion.h"
#include "Socket/UdpSocket.h"
#include "Util/ThreadUtils.h"


namespace
{

netlink::AdapterPriority mapPriority(const netlink::AdapterPriorityInternal internal)
{
	switch (internal)
	{
	case netlink::AdapterPriorityInternal::Preferred: return netlink::AdapterPriority::Preferred;
	case netlink::AdapterPriorityInternal::Available: return netlink::AdapterPriority::Available;
	default: return netlink::AdapterPriority::Suppressed;
	}
}

netlink::NetworkAdapter toPublicAdapter(const netlink::NetworkAdapterInternal &internal)
{
	netlink::NetworkAdapter pub;
	pub.adapterName = internal.AdapterName;
	pub.networkName = internal.NetworkName;
	pub.ipv4		= internal.IPv4;
	pub.id			= internal.ID;
	pub.priority	= mapPriority(internal.Priority);
	return pub;
}

} // namespace


netlink::NetLinkCore::NetLinkCore(NetLinkCoreDependencies dependencies) : mDependencies(std::move(dependencies)) {}


netlink::NetLinkCore::~NetLinkCore()
{
	stop();

	std::lock_guard<std::mutex> lock(mLifecycleMutex);
	finish();
}


std::shared_ptr<netlink::NetworkEngine> netlink::NetLinkCore::engine() const
{
	std::lock_guard<std::mutex> lock(mEngineMutex);
	return mEngine;
}


// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool netlink::NetLinkCore::start(const NetLinkConfig &config, const NetLinkCallbacks &callbacks)
{
	if (config.appId.empty())
		return false;

	// An engine that was stopped from a callback can only be cleaned up from another thread
	if (mEvents.isWorkerThread())
		return false;

	std::lock_guard<std::mutex> lock(mLifecycleMutex);

	if (const auto running = engine(); running && running->isRunning())
		return false;

	finish();

	// Log lines are written on every thread, the application reads them on the event thread
	mCallbacks = std::make_shared<const NetLinkCallbacks>(callbacks);
	internal::LogSink logSink;

	if (callbacks.onLog)
	{
		logSink = [this, callbacks = mCallbacks](const LogLevel level, std::string &&text)
		{ mEvents.post([callbacks, level, text = std::move(text)] { callbacks->onLog(level, text); }); };
	}

	{
		std::lock_guard<std::mutex> engineLock(mEngineMutex);
		mLogSink = logSink;
	}

	mEvents.start();

	if (logSink)
		mEvents.post([callbacks = mCallbacks] { internal::setThreadLogSink([callbacks](const LogLevel level, std::string &&text) { callbacks->onLog(level, text); }); });

	const internal::LogScope logScope(logSink);

	LocalInterfaceProvider localInterface = mDependencies.localInterface;

	if (!localInterface)
	{
		{
			std::lock_guard<std::mutex> networkLock(mNetworkMutex);
			enumerateAdapters();
			selectAdapter();
		}

		localInterface = [this] { return selectedInterface(); };
	}

	EngineConfig engineConfig;
	engineConfig.displayName		 = config.displayName;
	engineConfig.appId				 = config.appId;
	engineConfig.appVersion			 = config.appVersion;
	engineConfig.discoveryPort		 = config.discoveryPort;
	engineConfig.sendQueueBytes		 = config.sendQueueBytes;
	engineConfig.maxSendRate		 = config.maxSendRate;
	engineConfig.timings			 = mDependencies.timings;
	engineConfig.timings.peerTimeout = config.peerTimeout;
	engineConfig.autoAccept			 = !callbacks.onConnectionRequest;

	const auto engine				 = std::make_shared<NetworkEngine>(engineConfig, mDependencies.datagramSocketFactory, std::move(localInterface));

	NETLINK_LOG_INFO("NetLink {} starting as '{}' for application '{}'", internal::Version, config.displayName, config.appId);

	{
		std::lock_guard<std::mutex> engineLock(mEngineMutex);
		mEngine = engine;
	}

	internal::threadsStarted.fetch_add(1);
	mThread = std::jthread(
		[this, engine, logSink](const std::stop_token &stop)
		{
			internal::setThreadLogSink(logSink);
			engine->run(stop, [this](EventBatch &&batch) { deliver(std::move(batch)); });
		});

	return true;
}


void netlink::NetLinkCore::stop()
{
	const auto engine = this->engine();
	if (!engine)
		return;

	engine->shutdown();

	// A callback cannot wait for its own thread: the last events follow, the threads are joined later
	if (mEvents.isWorkerThread())
		return;

	std::lock_guard<std::mutex> lock(mLifecycleMutex);
	finish();
}


void netlink::NetLinkCore::finish()
{
	if (!engine())
		return;

	// The engine thread ends by itself once every peer was told
	joinOrDetach(mThread);
	mEvents.stopAfterDrain();

	std::lock_guard<std::mutex> lock(mEngineMutex);
	mEngine.reset();
	mLogSink = {};
}


netlink::internal::LogSink netlink::NetLinkCore::logSink() const
{
	std::lock_guard<std::mutex> lock(mEngineMutex);
	return mLogSink;
}


// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

void netlink::NetLinkCore::deliver(EventBatch &&batch)
{
	mEvents.post(
		[this, batch = std::make_shared<EventBatch>(std::move(batch)), callbacks = mCallbacks]
		{
			for (auto &event : batch->events)
				dispatch(event, *callbacks);
		});
}


void netlink::NetLinkCore::dispatch(EngineEvent &event, const NetLinkCallbacks &callbacks)
{
	switch (event.kind)
	{
	case EngineEvent::Kind::PeerDiscovered:
		if (callbacks.onPeerDiscovered)
			callbacks.onPeerDiscovered(event.info);
		break;

	case EngineEvent::Kind::PeerLost:
		if (callbacks.onPeerLost)
			callbacks.onPeerLost(event.peer);
		break;

	case EngineEvent::Kind::ConnectionRequest:
		if (callbacks.onConnectionRequest)
			callbacks.onConnectionRequest(event.info);
		break;

	case EngineEvent::Kind::Connected:
		if (callbacks.onConnected)
			callbacks.onConnected(event.info);
		break;

	case EngineEvent::Kind::Disconnected:
		if (callbacks.onDisconnected)
			callbacks.onDisconnected(event.peer, event.reason);
		break;

	case EngineEvent::Kind::Message:
		if (auto message = event.takeMessage(); message && callbacks.onMessage)
			callbacks.onMessage(event.peer, event.lane, std::move(*message));
		break;

	case EngineEvent::Kind::AdapterChanged:
		if (callbacks.onNetworkAdapterChanged)
			callbacks.onNetworkAdapterChanged(adapterAt(event.info.address));
		break;
	}
}


// ---------------------------------------------------------------------------
// Passed on to the engine
// ---------------------------------------------------------------------------

bool netlink::NetLinkCore::startDiscovery()
{
	const auto engine = this->engine();
	if (!engine || !engine->isRunning())
		return false;

	engine->setAnnouncing(true);
	return true;
}


void netlink::NetLinkCore::stopDiscovery()
{
	if (const auto engine = this->engine())
		engine->setAnnouncing(false);
}


std::vector<netlink::PeerInfo> netlink::NetLinkCore::peers() const
{
	const auto engine = this->engine();
	return engine ? engine->peers() : std::vector<PeerInfo>{};
}


std::vector<netlink::PeerId> netlink::NetLinkCore::connectedPeers() const
{
	const auto engine = this->engine();
	return engine ? engine->connectedPeers() : std::vector<PeerId>{};
}


bool netlink::NetLinkCore::connect(const PeerId peer)
{
	const auto engine = this->engine();
	return engine && engine->connect(peer);
}


void netlink::NetLinkCore::accept(const PeerId peer)
{
	if (const auto engine = this->engine())
		engine->accept(peer);
}


void netlink::NetLinkCore::decline(const PeerId peer)
{
	if (const auto engine = this->engine())
		engine->decline(peer);
}


void netlink::NetLinkCore::disconnect(const PeerId peer)
{
	if (const auto engine = this->engine())
		engine->disconnect(peer);
}


netlink::SendResult netlink::NetLinkCore::send(const PeerId peer, const uint32_t type, std::vector<uint8_t> &&data, const Lane lane, const std::chrono::milliseconds timeout)
{
	const auto engine = this->engine();
	return engine ? engine->send(peer, type, std::move(data), lane, timeout) : SendResult::NotRunning;
}


size_t netlink::NetLinkCore::broadcast(const uint32_t type, const std::span<const uint8_t> data, const Lane lane)
{
	const auto engine = this->engine();
	return engine ? engine->broadcast(type, data, lane) : 0;
}


std::optional<netlink::PeerStats> netlink::NetLinkCore::stats(const PeerId peer) const
{
	const auto engine = this->engine();
	return engine ? engine->stats(peer) : std::optional<PeerStats>{};
}


// ---------------------------------------------------------------------------
// Network adapters
// ---------------------------------------------------------------------------

void netlink::NetLinkCore::enumerateAdapters()
{
	if (mNetwork.init())
		mNetwork.processAdapter();
}


void netlink::NetLinkCore::selectAdapter()
{
	const auto &adapters = mNetwork.getAvailableNetworkAdapters();

	// The one chosen before, if it is still there
	if (mAdapter.isValid())
	{
		if (const auto current = mNetwork.isAdapterCurrentlyAvailable(mAdapter); current.isValid())
		{
			mAdapter = current;
			return;
		}
	}

	// Otherwise the best candidate: the application can still switch via setActiveAdapter()
	auto preferred = std::ranges::find_if(adapters, [](const auto &a) { return a.isValid() && a.Priority == AdapterPriorityInternal::Preferred; });

	if (preferred == adapters.end())
		preferred = std::ranges::find_if(adapters, [](const auto &a) { return a.isValid() && a.Priority == AdapterPriorityInternal::Available; });

	if (preferred != adapters.end())
		mAdapter = *preferred;
}


std::optional<netlink::LocalInterface> netlink::NetLinkCore::selectedInterface()
{
	std::lock_guard<std::mutex> lock(mNetworkMutex);

	const auto					interfaceOf = [](const NetworkAdapterInternal &adapter) -> std::optional<LocalInterface>
	{
		const auto ip = net::IPv4Address::parse(adapter.IPv4);
		if (!ip)
			return std::nullopt;

		return LocalInterface{.ip = *ip, .mask = net::IPv4Address::parse(adapter.Subnet).value_or(net::IPv4Address{})};
	};

	// Asked every few seconds from the I/O thread: as long as the address can still be bound, nothing else is looked up
	if (const auto current = interfaceOf(mAdapter); current && net::UdpSocket::bind({.ip = current->ip, .port = 0}))
		return current;

	if (!mAdapter.isValid())
		return std::nullopt;

	// The adapter lost its address: it may have a new one
	enumerateAdapters();

	const auto found = mNetwork.isAdapterCurrentlyAvailable(mAdapter);
	if (!found.isValid())
		return std::nullopt;

	mAdapter = found;
	return interfaceOf(mAdapter);
}


netlink::NetworkAdapter netlink::NetLinkCore::adapterAt(const std::string &ipv4) const
{
	std::lock_guard<std::mutex> lock(mNetworkMutex);

	if (mAdapter.isValid() && mAdapter.IPv4 == ipv4)
		return toPublicAdapter(mAdapter);

	NetworkAdapter adapter;
	adapter.ipv4 = ipv4;
	return adapter;
}


std::vector<netlink::NetworkAdapter> netlink::NetLinkCore::getAvailableAdapters()
{
	const internal::LogScope	logScope(logSink());
	std::lock_guard<std::mutex> lock(mNetworkMutex);
	enumerateAdapters();

	std::vector<NetworkAdapter> result;

	for (const auto &adapter : mNetwork.getAvailableNetworkAdapters())
		result.push_back(toPublicAdapter(adapter));

	return result;
}


bool netlink::NetLinkCore::setActiveAdapter(const uint64_t adapterID)
{
	const internal::LogScope logScope(logSink());

	{
		std::lock_guard<std::mutex> lock(mNetworkMutex);

		const auto				   &adapters = mNetwork.getAvailableNetworkAdapters();

		if (adapters.empty())
			enumerateAdapters();

		const auto chosen = std::ranges::find_if(adapters, [adapterID](const auto &adapter) { return static_cast<uint64_t>(adapter.ID) == adapterID; });

		if (chosen == adapters.end())
		{
			NETLINK_LOG_WARNING("No adapter found with ID {}", adapterID);
			return false;
		}

		mAdapter = *chosen;
	}

	// The engine moves to the new address with its next step
	if (const auto engine = this->engine())
		engine->checkInterface();

	return true;
}


uint64_t netlink::NetLinkCore::getActiveAdapterID() const
{
	std::lock_guard<std::mutex> lock(mNetworkMutex);
	return mAdapter.ID;
}
