/*
  ==============================================================================
	Module:         DiscoveryService
	Description:    LAN discovery via UDP broadcast
  ==============================================================================
*/

#include "DiscoveryService.h"
#include "NetLinkLog.h"

using json = nlohmann::json;


DiscoveryService::~DiscoveryService()
{
	try
	{
		deinit();
	}
	catch (const std::exception &e)
	{
		NETLINK_LOG_ERROR("Exception during DiscoveryService destruction: {}", e.what());
	}
	catch (...)
	{
		NETLINK_LOG_ERROR("Unknown exception during DiscoveryService destruction");
	}
}


bool DiscoveryService::init(const DiscoveryConfig &config)
{
	mConfig = config;
	mSocket = NetlinkSocket::createUDP();

	NetLink::BindOptions options;
	options.enableBroadcast = true;
	options.reuseAddress	= true;

	auto result				= mSocket.bind(config.localIPv4, config.discoveryPort, options);
	if (!result.succeeded())
		return false;

	mInitialized.store(true);
	start();
	return true;
}


void DiscoveryService::deinit()
{
	stop();
	mSocket.close();
	mInitialized.store(false);
}


void DiscoveryService::startDiscovery()
{
	if (!isInitialized())
	{
		throw std::runtime_error("Discovery Service has not been initialized but was called to start!");
		return;
	}

	start();
}


DiscoveryEndpoint DiscoveryService::getEndpointFromIP(const std::string &IPv4)
{
	for (auto &endpoint : mRemoteDevices)
	{
		if (endpoint.IPAddress == IPv4)
			return endpoint;
	}
	return {};
}


void DiscoveryService::addRemoteToList(DiscoveryEndpoint remote)
{
	if (!remote.isValid())
		return;

	for (const auto &ep : mRemoteDevices)
	{
		if (ep == remote)
			return;
	}

	if (mConfig.localIPv4 == remote.IPAddress)
		return;

	NETLINK_LOG_INFO("Found remote: IP={}, Port={}, Name={}", remote.IPAddress.c_str(), remote.port, remote.displayName.c_str());

	mRemoteDevices.push_back(remote);

	if (mOnRemoteFound)
		mOnRemoteFound(remote);
}


void DiscoveryService::run()
{
	mNextSendTime = std::chrono::steady_clock::now();

	while (isRunning())
	{
		if (std::chrono::steady_clock::now() >= mNextSendTime)
		{
			sendPackage();
			mNextSendTime = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		}

		receivePackage(); // blocks internally via mSocket.receive(...)
	}
}


void DiscoveryService::sendPackage()
{
	if (!isInitialized())
	{
		NETLINK_LOG_ERROR("Tried to start the Discovery in Server mode without initializing first! Please initialize before attempting to start the Discovery in Server Mode!");
		return;
	}

	DiscoveryEndpoint local{};
	local.IPAddress		= mConfig.localIPv4;
	local.displayName	= mConfig.displayName;
	local.port			= mConfig.signalingPort;

	json		j		= local;
	std::string message = j.dump();

	mSocket.sendTo(mTargetAddress, mTargetPort, message.data(), static_cast<int>(message.size()));
}


void DiscoveryService::receivePackage()
{
	ReceivedPacket packet;
	if (!mSocket.receive(packet, 200))
		return; // nothing received this cycle

	try
	{
		json			  j		 = json::parse(packet);
		DiscoveryEndpoint remote = j.get<DiscoveryEndpoint>();
		addRemoteToList(remote);

		if (mOnRemoteFound)
			mOnRemoteFound(remote);
	}
	catch (std::exception &e)
	{
		NETLINK_LOG_ERROR("Error parsing discovery package: {}", e.what());
	}
}
