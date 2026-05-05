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


netlink::NetLink::NetLink() {}


netlink::NetLink::~NetLink() {}


netlink::NetLink::NetLink(NetLink &&) noexcept {}


netlink::NetLink &netlink::NetLink::operator=(NetLink &&) noexcept = default;


void			  netlink::NetLink::configure(const NetLinkConfig &config, const NetLinkCallbacks &callbacks) {}


bool			  netlink::NetLink::init()
{
	return false;
}


void netlink::NetLink::shutdown() {}


bool netlink::NetLink::startDiscovery()
{
	return false;
}


void						   netlink::NetLink::stopDiscovery() {}


std::vector<netlink::Endpoint> netlink::NetLink::getDiscoveredEndpoints()
{
	return std::vector<Endpoint>();
}


bool netlink::NetLink::hostSession()
{
	return false;
}


bool netlink::NetLink::connectTo(const Endpoint &remote)
{
	return false;
}


void					 netlink::NetLink::respondToConnection(bool accepted) {}


void					 netlink::NetLink::disconnect() {}


netlink::ConnectionState netlink::NetLink::getConnectionState() const
{
	return ConnectionState();
}


bool netlink::NetLink::send(const Message &message)
{
	return false;
}


bool netlink::NetLink::send(uint32_t type, const std::vector<uint8_t> &payload)
{
	return false;
}


std::vector<netlink::NetworkAdapter> netlink::NetLink::getAvailableAdapters()
{
	return std::vector<NetworkAdapter>();
}


bool netlink::NetLink::setActiveAdapter(const int &adapterID)
{
	return false;
}
