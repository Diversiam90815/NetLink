/*
==============================================================================
	Module:         NetLink
	Description:    API for NetLink library
  ==============================================================================
*/

#include "NetLink/NetLink.h"

#include "Discovery/DiscoveryService.h"
#include "Signaling/SignalingService.h"
#include "Signaling/RoleNegotiation.h"
#include "TCP/TCPSession.h"
#include "Messaging/RemoteCommunication.h"


struct netlink::NetLink::Impl
{
	NetLinkConfig		config;
	NetLinkCallbacks	callbacks;

	asio::io_context	ioContext;
	DiscoveryService	discovery{ioContext};
	SignalingService	signaling{ioContext};
	RemoteCommunication communication;

	// Pending inbound request info (for respondToConnetion)
	DiscoveryEndpoint	pendingRemote;

	ConnectionState		connectionState{ConnectionState::None};
};


netlink::NetLink::NetLink() : pImpl(std::make_unique<Impl>()) {}


netlink::NetLink::~NetLink()
{
	shutdown();
}


netlink::NetLink::NetLink(NetLink &&) noexcept {}


netlink::NetLink &netlink::NetLink::operator=(NetLink &&) noexcept = default;


void			  netlink::NetLink::configure(const NetLinkConfig &config, const NetLinkCallbacks &callbacks) {}


bool			  netlink::NetLink::init()
{
	return false;
}


void netlink::NetLink::shutdown()
{
	pImpl->communication.deinit();
	pImpl->signaling.deinit();
	pImpl->discovery.deinit();
	pImpl->connectionState = ConnectionState::None;
}


bool netlink::NetLink::startDiscovery()
{
	pImpl->discovery.startDiscovery();
	pImpl->signaling.start();
	pImpl->connectionState = ConnectionState::Searching;
	return true;
}


void netlink::NetLink::stopDiscovery()
{
	pImpl->discovery.deinit();
}


std::vector<netlink::Endpoint> netlink::NetLink::getDiscoveredEndpoints()
{
	// TODO: map internal endpoints to public endpoints
	return std::vector<Endpoint>();
}


bool netlink::NetLink::connectTo(const Endpoint &remote)
{
	return false;
}


void netlink::NetLink::respondToConnection(bool accepted)
{
	auto &pending = pImpl->pendingRemote;

	if (accepted)
	{
		pImpl->signaling.sendConnectAccept(pending.IPAddress, pending.signalingPort);

		// TODO: Enter flow of establishing connection
	}
	else
	{
		pImpl->signaling.sendConnectDecline(pending.IPAddress, pending.signalingPort);
		pImpl->connectionState = ConnectionState::None;
	}
}


void netlink::NetLink::disconnect()
{
	// signal the remote peer before teardown
	auto &pending = pImpl->pendingRemote;

	if (pending.isValid())
		pImpl->signaling.sendDisconnect(pending.IPAddress, pending.signalingPort);

	pImpl->communication.deinit();
	pImpl->connectionState = ConnectionState::Disconnected;
}


netlink::ConnectionState netlink::NetLink::getConnectionState() const
{
	return pImpl->connectionState;
}


bool netlink::NetLink::send(const Message &message)
{
	return send(message.type, message.data);
}


bool netlink::NetLink::send(uint32_t type, const std::vector<uint8_t> &payload)
{
	if (pImpl->connectionState != ConnectionState::Connected)
		return false;

	pImpl->communication.write(type, payload);
	return true;
}


std::vector<netlink::NetworkAdapter> netlink::NetLink::getAvailableAdapters()
{
	return std::vector<NetworkAdapter>();
}


bool netlink::NetLink::setActiveAdapter(const int &adapterID)
{
	return false;
}
