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
#include "Transport/TransportInterfaces.h"
#include "TCP/TCPTransportFactory.h"
#include "Messaging/RemoteCommunication.h"
#include "ConnectionFlow/ConnectionFlow.h"


struct netlink::NetLink::Impl
{
	NetLinkConfig		config;
	NetLinkCallbacks	callbacks;

	asio::io_context		  ioContext;
	DiscoveryService		  discovery{ioContext};
	SignalingService		  signaling{ioContext};
	netlink::TCPTransportFactory transportFactory;
	ConnectionFlow			  connectionFlow{ioContext, signaling, transportFactory};
	RemoteCommunication		  communication;

	ConnectionState		connectionState{ConnectionState::None};
};


netlink::NetLink::NetLink() : pImpl(std::make_unique<Impl>()) {}


netlink::NetLink::~NetLink()
{
	shutdown();
}


netlink::NetLink::NetLink(NetLink &&) noexcept {}


netlink::NetLink &netlink::NetLink::operator=(NetLink &&) noexcept = default;


void			  netlink::NetLink::configure(const NetLinkConfig &config, const NetLinkCallbacks &callbacks)
{
	pImpl->config	 = config;
	pImpl->callbacks = callbacks;
	pImpl->connectionFlow.setLocalIP(config.localIPv4);

	ConnectionFlowCallbacks flowCB;

	// Remote wants to connect -> ask app to accept/decline
	flowCB.onConnectionRequested = [this](const DiscoveryEndpoint &remote)
	{
		pImpl->connectionState = ConnectionState::None;
		if (pImpl->callbacks.onConnectionChanged)
			pImpl->callbacks.onConnectionChanged({ConnectionState::None, "", {remote.IPAddress, remote.port, remote.displayName}});
	};

	// Transport established -> init messaging
	flowCB.onConnected = [this](ISession::pointer session)
	{
		pImpl->communication.init(session, pImpl->config.secret);
		pImpl->communication.start();
		pImpl->connectionState = ConnectionState::Connected;

		if (pImpl->callbacks.onConnectionChanged)
			pImpl->callbacks.onConnectionChanged({ConnectionState::Connected, "", {}});
	};

	flowCB.onConnectionFailed = [this](const std::string &reason)
	{
		pImpl->connectionState = ConnectionState::Error;
		if (pImpl->callbacks.onConnectionChanged)
			pImpl->callbacks.onConnectionChanged({ConnectionState::Error, reason, {}});
	};

	flowCB.onDisconnected = [this]()
	{
		pImpl->communication.deinit();
		pImpl->connectionState = ConnectionState::Disconnected;
		if (pImpl->callbacks.onConnectionChanged)
			pImpl->callbacks.onConnectionChanged({ConnectionState::Disconnected, "", {}});
	};

	pImpl->connectionFlow.setCallbacks(std::move(flowCB));
}


bool netlink::NetLink::init()
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
	// Map public endpoint to internal DiscoveryEndpoint
	DiscoveryEndpoint ep{remote.IPAddress, remote.port, remote.port, remote.displayName}; // @TODO: signalingport
	pImpl->connectionFlow.requestConnection(ep);
	
	pImpl->connectionState = ConnectionState::Searching;

	return true;
}


void netlink::NetLink::respondToConnection(bool accepted)
{
	pImpl->connectionFlow.respondToConnection(accepted);
}


void					 netlink::NetLink::disconnect() {}


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
