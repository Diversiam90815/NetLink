/*
  ==============================================================================
	Module:         DiscoveryService
	Description:    LAN discovery via UDP broadcast
  ==============================================================================
*/

#include "DiscoveryService.h"
#include "NetLinkLog.h"

using json = nlohmann::json;


DiscoveryService::DiscoveryService(asio::io_context &ioContext) : mSocket(ioContext), mTimer(ioContext)
{
	mIoContext = &ioContext;
}


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
	if (config.localIPv4.empty() || config.displayName.empty())
		return false;

	const bool needsRebind = !mInitialized.load() || mConfig.localIPv4 != config.localIPv4 || mConfig.discoveryPort != config.discoveryPort;

	mConfig				   = config;

	if (!needsRebind)
	{
		NETLINK_LOG_DEBUG("DiscoveryService config updated (no rebind required)");
		return true;
	}

	asio::error_code ec;
	mSocket.open(udp::v4());

	mSocket.set_option(asio::socket_base::reuse_address(true), ec);

	if (ec)
		NETLINK_LOG_ERROR("Failed to set reuse_address option: {}", ec.message().c_str());

	mLocalEndpoint	= udp::endpoint(udp::v4(), mConfig.discoveryPort);
	mTargetEndpoint = udp::endpoint(asio::ip::make_address_v4(mConfig.broadCastAddress), mConfig.discoveryPort);

	mSocket.bind(mLocalEndpoint, ec);

	if (ec)
	{
		NETLINK_LOG_ERROR("Failed to bind UDP Socket: {}", ec.message().c_str());
		return false;
	}

	mSocket.set_option(asio::socket_base::broadcast(true));

	mInitialized.store(true);
	return true;
}


void DiscoveryService::deinit()
{
	asio::error_code ec;

	// Cancel any pending async operations on the socket
	mSocket.cancel(ec);

	if (ec)
		NETLINK_LOG_ERROR("Error cancelling socket operations: {}", ec.message().c_str());

	mSocket.close(ec);

	if (ec)
		NETLINK_LOG_ERROR("Error occurred during closing of the socket! Error : {}", ec.message().c_str());

	mTimer.cancel();
	stop();
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
	while (isRunning())
	{
		receivePackage();
		sendPackage();
		waitForEvent(200);
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
	local.IPAddress			 = mConfig.localIPv4;
	local.displayName		 = mConfig.displayName;
	local.port				 = mConfig.signalingPort;

	json			 j		 = local;
	std::string		 message = j.dump();

	asio::error_code ec;

	// Sending the message
	size_t			 bytesSent = mSocket.send_to(asio::buffer(message), mTargetEndpoint, 0, ec);

	if (ec)
		NETLINK_LOG_ERROR("Error sending discovery package: {}", ec.message().c_str());
	else
		NETLINK_LOG_DEBUG("Discovery package sent ({} bytes)!", bytesSent);
}


void DiscoveryService::receivePackage()
{
	mSocket.async_receive(asio::buffer(mRecvBuffer), [this](const asio::error_code &error, size_t bytesReceived) { handleReceive(error, bytesReceived); });
}


void DiscoveryService::handleReceive(const asio::error_code &error, size_t bytesReceived)
{
	if (!error && bytesReceived > 0)
	{
		try
		{
			std::string		  receivedData(mRecvBuffer.data(), bytesReceived);
			json			  j		 = json::parse(receivedData);

			DiscoveryEndpoint remote = j.get<DiscoveryEndpoint>();

			addRemoteToList(remote);
		}
		catch (std::exception &e)
		{
			NETLINK_LOG_ERROR("Error parsing discovery package: {}", e.what());
		}
	}
	else if (error)
	{
		NETLINK_LOG_WARNING("Receive error occurred: {}", error.message().c_str());
	}
}
