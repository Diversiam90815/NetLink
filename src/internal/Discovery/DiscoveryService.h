/*
  ==============================================================================
	Module:         DiscoveryService
	Description:    LAN discovery via UDP broadcast
  ==============================================================================
*/

#pragma once

#include <asio.hpp>
#include <asio/steady_timer.hpp>
#include <asio/ip/udp.hpp>

#include <string>
#include <functional>
#include <vector>
#include <array>
#include <atomic>

#include "DiscoveryEndpoint.h"
#include "ThreadBase.h"


using asio::ip::udp;


struct DiscoveryConfig
{
	std::string	   displayName{};
	std::string	   localIPv4{};
	int			   discoveryPort{5555};
	std::string	   broadCastAddress{"255.255.255.255"};
};


// @brief		Callback signature when a remote endpoint is discovered
using RemoteFoundCallback = std::function<void(const DiscoveryEndpoint &)>;


/**
 * @brief	Provides LAN discovery via UDP broadcast.
 */
class DiscoveryService : public ThreadBase
{
public:
	DiscoveryService(asio::io_context &ioContext);
	~DiscoveryService();

	// @brief		Set callback invoked when a remote is discovered
	void			  setOnRemoteFound(RemoteFoundCallback cb) { mOnRemoteFound = std::move(cb); }

	// @brief		Initialize socket and bind to discovery port.
	bool			  init(const DiscoveryConfig &config);

	// @brief		Tear down socket and stop thread. Safe to call multiple times.
	void			  deinit();

	// @brief		Begin broadcasting
	void			  startDiscovery();

	// @brief		Look up a previously discovered endpoint by IP.
	DiscoveryEndpoint getEndpointFromIP(const std::string &IPv4);

	// @brief		Manually add a remote endpoint (duplicates are filtered).
	void			  addRemoteToList(DiscoveryEndpoint remote);


private:
	void						   run() override;

	void						   sendPackage();

	void						   receivePackage();

	void						   handleReceive(const asio::error_code &error, size_t bytesReceived);

	bool						   isInitialized() const { return mInitialized.load(); }


	DiscoveryConfig				   mConfig;
	std::atomic<bool>			   mInitialized{false};

	asio::io_context			  *mIoContext = nullptr;
	udp::socket					   mSocket;

	udp::endpoint				   mLocalEndpoint;
	udp::endpoint				   mTargetEndpoint;
	std::vector<DiscoveryEndpoint> mRemoteDevices;

	std::array<char, 1024>		   mRecvBuffer{};
	asio::steady_timer			   mTimer;

	RemoteFoundCallback			   mOnRemoteFound;
};
