/*
  ==============================================================================
	Module:         DiscoveryService
	Description:    LAN discovery via UDP broadcast
  ==============================================================================
*/

#include "DiscoveryService.h"

#include <algorithm>
#include <span>

#include "NetLinkConstants.h"
#include "NetLinkLog.h"
#include "Socket/UdpSocket.h"

using json = nlohmann::json;

namespace
{
netlink::net::IPv4Address broadcastTarget(const DiscoveryConfig &config)
{
	if (config.broadcastAddress.isBroadcast() && config.subnetMask.isNetmask() && !config.localIPv4.isUnspecified())
		return netlink::net::subnetBroadcast(config.localIPv4, config.subnetMask);

	return config.broadcastAddress;
}
} // namespace


DiscoveryService::DiscoveryService(netlink::net::DatagramSocketFactory socketFactory)
	: mSocketFactory(socketFactory ? std::move(socketFactory) : netlink::net::UdpSocket::factory()), mReceiveBuffer(netlink::internal::PackageBufferSize)
{
}


DiscoveryService::~DiscoveryService()
{
	deinit();
}


void DiscoveryService::setOnRemoteFound(RemoteFoundCallback cb)
{
	std::lock_guard<std::mutex> lock(mMutex);
	mOnRemoteFound = std::move(cb);
}


void DiscoveryService::setOnRemoteLost(RemoteLostCallback cb)
{
	std::lock_guard<std::mutex> lock(mMutex);
	mOnRemoteLost = std::move(cb);
}


bool DiscoveryService::init(const DiscoveryConfig &config)
{
	if (config.localIPv4.isUnspecified() || config.displayName.empty() || config.discoveryPort <= 0 || config.discoveryPort > 65535)
		return false;

	{
		std::lock_guard<std::mutex> lock(mMutex);

		if (mSocket && mConfig.discoveryPort == config.discoveryPort)
		{
			mConfig = config;
			mAnnounceRequested.store(true);
			return true;
		}
	}

	netlink::net::BindOptions options;
	options.enableBroadcast = true;
	options.reuseAddress	= true;

	auto socket				= mSocketFactory(netlink::net::SocketAddress::any(static_cast<uint16_t>(config.discoveryPort)), options);

	if (!socket)
	{
		NETLINK_LOG_ERROR("DiscoveryService: binding port {} failed: {}", config.discoveryPort, netlink::net::toString(socket.error()));
		return false;
	}

	std::shared_ptr<netlink::net::IDatagramSocket> previous;

	{
		std::lock_guard<std::mutex> lock(mMutex);
		previous = std::exchange(mSocket, std::shared_ptr<netlink::net::IDatagramSocket>(std::move(*socket)));
		mConfig	 = config;
	}

	if (previous)
		previous->shutdown();

	mAnnounceRequested.store(true);
	return true;
}


void DiscoveryService::deinit()
{
	ThreadBase::stop();

	std::lock_guard<std::mutex> lock(mMutex);

	if (mSocket)
		mSocket->shutdown();

	mSocket.reset();
	mRemoteDevices.clear();
}


DiscoveryConfig DiscoveryService::getConfig() const
{
	std::lock_guard<std::mutex> lock(mMutex);
	return mConfig;
}


bool DiscoveryService::startDiscovery()
{
	if (!socket())
	{
		NETLINK_LOG_ERROR("Discovery cannot start: the service has not been initialised");
		return false;
	}

	ThreadBase::start();
	return true;
}


void DiscoveryService::stopDiscovery()
{
	ThreadBase::stop();
}


DiscoveryEndpoint DiscoveryService::getEndpointFromIP(const netlink::net::IPv4Address &IPv4)
{
	std::lock_guard<std::mutex> lock(mMutex);

	auto						it = std::ranges::find_if(mRemoteDevices, [&](const KnownPeer &peer) { return peer.endpoint.IPAddress == IPv4; });
	return it != mRemoteDevices.end() ? it->endpoint : DiscoveryEndpoint{};
}


void DiscoveryService::addRemoteToList(DiscoveryEndpoint remote)
{
	if (!remote.isValid())
		return;

	RemoteFoundCallback callback;

	{
		std::lock_guard<std::mutex> lock(mMutex);

		if (mConfig.localIPv4 == remote.IPAddress)
			return;

		const auto now = std::chrono::steady_clock::now();

		auto	   it  = std::ranges::find_if(mRemoteDevices, [&](const KnownPeer &peer) { return peer.endpoint.IPAddress == remote.IPAddress; });

		if (it != mRemoteDevices.end())
		{
			it->lastSeen = now;

			if (it->endpoint == remote && it->endpoint.displayName == remote.displayName)
				return;			   // periodic re-announcement

			NETLINK_LOG_INFO("Remote updated: IP={}, Port={}, Name={}", remote.IPAddress.toString(), remote.port, remote.displayName);
			it->endpoint = remote; // e.g. the remote rebound its signaling socket
		}
		else
		{
			NETLINK_LOG_INFO("Found remote: IP={}, Port={}, Name={}", remote.IPAddress.toString(), remote.port, remote.displayName);
			mRemoteDevices.push_back({remote, now});
		}

		callback = mOnRemoteFound;
	}

	if (callback)
		callback(remote);
}


std::shared_ptr<netlink::net::IDatagramSocket> DiscoveryService::socket() const
{
	std::lock_guard<std::mutex> lock(mMutex);
	return mSocket;
}


void DiscoveryService::run()
{
	mNextSendTime = std::chrono::steady_clock::now();

	while (isRunning())
	{
		if (mAnnounceRequested.exchange(false) || std::chrono::steady_clock::now() >= mNextSendTime)
		{
			sendPackage();
			mNextSendTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(mConfig.announceIntervalMS);
		}

		expireStalePeers();
		receivePackage();
	}
}


void DiscoveryService::expireStalePeers()
{
	std::vector<DiscoveryEndpoint> lost;
	RemoteLostCallback			   callback;

	{
		std::lock_guard<std::mutex> lock(mMutex);

		const auto					deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(mConfig.peerTimeoutMs);

		for (auto it = mRemoteDevices.begin(); it != mRemoteDevices.end();)
		{
			if (it->lastSeen < deadline)
			{
				NETLINK_LOG_INFO("Remote {} stopped announcing, dropping it", it->endpoint.displayName);
				lost.push_back(it->endpoint);
				it = mRemoteDevices.erase(it);
			}
			else
				++it;
		}

		callback = mOnRemoteLost;
	}

	// Outside the lock: the callback may call back into this service
	if (callback)
	{
		for (const auto &endpoint : lost)
			callback(endpoint);
	}
}


void DiscoveryService::sendPackage()
{
	auto socket = this->socket();

	if (!socket)
		return;

	DiscoveryEndpoint local{};
	local.IPAddress			  = mConfig.localIPv4;
	local.displayName		  = mConfig.displayName;
	local.port				  = mConfig.signalingPort;

	const std::string message = json(local).dump();
	const auto		  target  = netlink::net::SocketAddress{broadcastTarget(mConfig), static_cast<uint16_t>(mConfig.discoveryPort)};

	if (auto sent = socket->sendTo(target, std::span(reinterpret_cast<const uint8_t *>(message.data()), message.size())); !sent)
		NETLINK_LOG_WARNING("DiscoveryService: announcing to {} failed: {}", target.toString(), netlink::net::toString(sent.error()));
}


void DiscoveryService::receivePackage()
{
	auto socket = this->socket();

	if (!socket)
	{
		waitForEvent(static_cast<unsigned long>(netlink::internal::SocketPollInterval.count()));
		return;
	}

	auto datagram = socket->receiveFrom(mReceiveBuffer, netlink::internal::SocketPollInterval);

	if (!datagram)
	{
		if (datagram.error() != netlink::net::SocketError::Timeout)
			waitForEvent(static_cast<unsigned long>(netlink::internal::SocketPollInterval.count()));

		return;
	}

	if (mConfig.subnetMask.isNetmask() && !mConfig.localIPv4.isUnspecified())
	{
		if (!netlink::net::sameSubnet(mConfig.localIPv4, datagram->from.ip, mConfig.subnetMask))
		{
			NETLINK_LOG_DEBUG("Ignoring announcement from {}: outside the selected subnet", datagram->from.toString());
			return;
		}
	}

	try
	{
		const auto		  begin	 = mReceiveBuffer.data();
		auto			  j		 = json::parse(begin, begin + datagram->size);
		DiscoveryEndpoint remote = j.get<DiscoveryEndpoint>();
		addRemoteToList(remote);
	}
	catch (const std::exception &e)
	{
		NETLINK_LOG_ERROR("Error parsing discovery package from {}: {}", datagram->from.toString(), e.what());
	}
}
