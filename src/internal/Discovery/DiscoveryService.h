/*
  ==============================================================================
	Module:         DiscoveryService
	Description:    LAN discovery via UDP broadcast.
  ==============================================================================
*/

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "DiscoveryEndpoint.h"
#include "DiscoveryRegistry.h"
#include "ThreadBase.h"
#include "Socket/IDatagramSocket.h"


struct DiscoveryConfig
{
	std::string				  displayName{};
	netlink::net::IPv4Address localIPv4{};
	int						  signalingPort{0};
	int						  discoveryPort{5555};
	netlink::net::IPv4Address broadcastAddress{netlink::net::IPv4Address::broadcast()};
	int						  peerTimeoutMs{6000};
	int						  announceIntervalMS{2000};

	netlink::net::IPv4Address subnetMask{}; // When set, inbound announcements from other subnets are ignored
};


using RemoteFoundCallback = std::function<void(const DiscoveryEndpoint &)>;
using RemoteLostCallback  = std::function<void(const DiscoveryEndpoint &)>;


class DiscoveryService : private ThreadBase
{
public:
	explicit DiscoveryService(netlink::net::DatagramSocketFactory socketFactory = {});
	~DiscoveryService() override;
	DiscoveryService(const DiscoveryService &)			  = delete;
	DiscoveryService &operator=(const DiscoveryService &) = delete;

	// Invoked on the discovery thread for every new (or changed) remote
	void			  setOnRemoteFound(RemoteFoundCallback cb);

	// Invoked on the discovery thread when a remote stopped announcing for peerTimeoutMs
	void			  setOnRemoteLost(RemoteLostCallback cb);

	// Applies the configuration. Rebinds the socket only if the discovery port changed. Safe while discovering.
	bool			  init(const DiscoveryConfig &config);
	void			  deinit();

	DiscoveryConfig	  getConfig() const;

	bool			  startDiscovery();
	void			  stopDiscovery();
	bool			  isDiscovering() const { return isRunning(); }

	DiscoveryEndpoint getEndpointFromIP(const netlink::net::IPv4Address &IPv4);
	void			  addRemoteToList(DiscoveryEndpoint remote);


private:
	void										   run() override;

	void										   sendPackage();
	void										   receivePackage();
	void										   expireStalePeers();

	std::shared_ptr<netlink::net::IDatagramSocket> socket() const;

	netlink::net::DatagramSocketFactory			   mSocketFactory;

	mutable std::mutex							   mMutex;
	DiscoveryConfig								   mConfig;
	std::shared_ptr<netlink::net::IDatagramSocket> mSocket;

	netlink::discovery::DiscoveryRegistry		   mRegistry;

	RemoteFoundCallback							   mOnRemoteFound;
	RemoteLostCallback							   mOnRemoteLost;

	std::atomic<bool>							   mAnnounceRequested{false};

	// Only touched by the discovery thread
	std::vector<uint8_t>						   mReceiveBuffer;
	std::chrono::steady_clock::time_point		   mNextSendTime;
};
