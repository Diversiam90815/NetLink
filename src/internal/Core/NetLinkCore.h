/*
  ==============================================================================
	Module:         NetLinkCore
	Description:    Runs the engine on its thread, delivers its events on the
					event thread and tells it which network adapter to use
  ==============================================================================
*/

#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "NetLink/NetLink.h"

#include "Engine/NetworkEngine.h"
#include "Network/NetworkInformation.h"
#include "Socket/IDatagramSocket.h"
#include "Util/TaskQueue.h"


namespace netlink
{

struct NetLinkCoreDependencies
{
	net::DatagramSocketFactory datagramSocketFactory{}; // empty = real UDP sockets
	LocalInterfaceProvider	   localInterface{};		// empty = the network adapters of this machine
	channel::LinkTimings	   timings{};				// of every link. peerTimeout comes from the configuration.
};


class NetLinkCore
{
public:
	explicit NetLinkCore(NetLinkCoreDependencies dependencies = {});
	~NetLinkCore();

	NetLinkCore(const NetLinkCore &)							  = delete;
	NetLinkCore					  &operator=(const NetLinkCore &) = delete;

	bool						   start(const NetLinkConfig &config, const NetLinkCallbacks &callbacks);

	// From a callback: only asks the engine to stop. The threads are joined by the next stop() from elsewhere.
	void						   stop();

	bool						   startDiscovery();
	void						   stopDiscovery();

	std::vector<PeerInfo>		   peers() const;
	std::vector<PeerId>			   connectedPeers() const;

	bool						   connect(PeerId peer);
	void						   accept(PeerId peer);
	void						   decline(PeerId peer);
	void						   disconnect(PeerId peer);

	SendResult					   send(PeerId peer, uint32_t type, std::vector<uint8_t> &&data, Lane lane, std::chrono::milliseconds timeout);
	size_t						   broadcast(uint32_t type, std::span<const uint8_t> data, Lane lane);
	std::optional<PeerStats>	   stats(PeerId peer) const;

	std::vector<NetworkAdapter>	   getAvailableAdapters();
	bool						   setActiveAdapter(uint64_t adapterID);
	uint64_t					   getActiveAdapterID() const;

	// The running engine, null if there is none
	std::shared_ptr<NetworkEngine> engine() const;

private:
	// Joins the threads of an engine that was asked to stop. Caller holds mLifecycleMutex.
	void									finish();

	void									deliver(EventBatch &&batch);
	void									dispatch(EngineEvent &event, const NetLinkCallbacks &callbacks);

	// Looks the adapters of this machine up again
	void									enumerateAdapters();
	void									selectAdapter();
	std::optional<LocalInterface>			selectedInterface();
	NetworkAdapter							adapterAt(const std::string &ipv4) const;


	NetLinkCoreDependencies					mDependencies;

	std::mutex								mLifecycleMutex; // start() and stop() one at a time
	mutable std::mutex						mEngineMutex;
	std::shared_ptr<NetworkEngine>			mEngine;
	std::shared_ptr<const NetLinkCallbacks> mCallbacks;
	std::jthread							mThread;
	TaskQueue								mEvents;

	// The adapters of this machine. Unused when the address comes from the dependencies.
	mutable std::mutex						mNetworkMutex;
	NetworkInformation						mNetwork;
	NetworkAdapterInternal					mAdapter; // the selected one, as it was last seen
};

} // namespace netlink
