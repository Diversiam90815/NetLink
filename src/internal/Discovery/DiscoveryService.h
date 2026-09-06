/*
  ==============================================================================
	Module:         DiscoveryService
	Description:    LAN discovery via UDP broadcast
  ==============================================================================
*/

#pragma once

#include <string>
#include <functional>
#include <vector>
#include <array>
#include <atomic>

#include "DiscoveryEndpoint.h"
#include "ThreadBase.h"
#include "../Socket/NetlinkSocket.h"


struct DiscoveryConfig
{
	std::string displayName{};
	std::string localIPv4{};
	int			signalingPort{0};
	int			discoveryPort{5555};
	std::string broadCastAddress{"255.255.255.255"};
};


using RemoteFoundCallback = std::function<void(const DiscoveryEndpoint &)>;


// Provides LAN discovery via UDP broadcast.
class DiscoveryService : public ThreadBase
{
public:
	DiscoveryService() = default;
	~DiscoveryService();

	void				   setOnRemoteFound(RemoteFoundCallback cb) { mOnRemoteFound = std::move(cb); }

	bool				   init(const DiscoveryConfig &config);
	void				   deinit();

	const DiscoveryConfig &getConfig() const { return mConfig; }

	void				   startDiscovery();

	DiscoveryEndpoint	   getEndpointFromIP(const std::string &IPv4);
	void				   addRemoteToList(DiscoveryEndpoint remote);


private:
	void								  run() override;

	void								  sendPackage();
	void								  receivePackage();

	bool								  isInitialized() const { return mInitialized.load(); }


	DiscoveryConfig						  mConfig;
	std::atomic<bool>					  mInitialized{false};

	NetlinkSocket						  mSocket; // UDP

	std::string							  mLocalAddress;
	std::string							  mTargetAddress;
	int									  mTargetPort = 0;

	std::vector<DiscoveryEndpoint>		  mRemoteDevices;

	std::chrono::steady_clock::time_point mNextSendTime;

	RemoteFoundCallback					  mOnRemoteFound;
};
