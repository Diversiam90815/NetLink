/*
==============================================================================
	Module:         NetLink
	Description:    API for NetLink library
  ==============================================================================
*/

#include "NetLink/NetLink.h"

#include "Discovery/DiscoveryService.h"
#include "Signaling/SignalingService.h"
#include "Transport/TransportInterfaces.h"
#include "TCP/TCPTransportFactory.h"
#include "Messaging/RemoteCommunication.h"
#include "ConnectionService/ConnectionService.h"
#include "PeerValidation/PeerValidationService.h"


struct netlink::NetLink::Impl
{
	NetLinkConfig				   config;
	NetLinkCallbacks			   callbacks;

	asio::io_context			   ioContext;
	DiscoveryService			   discovery{ioContext};
	SignalingService			   signaling{ioContext};
	netlink::TCPTransportFactory   transportFactory;
	netlink::PeerValidationService validation;
	netlink::ConnectionService	   connectionService{ioContext, signaling, transportFactory, validation};
	RemoteCommunication			   communication;

	ConnectionState				   connectionState{ConnectionState::None};
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
	pImpl->connectionService.setLocalIP(config.localIPv4);

	netlink::ConnectionServiceCallbacks svcCB;

	// Remote wants to connect -> ask app to accept/decline
	svcCB.onConnectionRequested = [this](const DiscoveryEndpoint &remote)
	{
		pImpl->connectionState = ConnectionState::None;
		if (pImpl->callbacks.onConnectionChanged)
			pImpl->callbacks.onConnectionChanged({ConnectionState::None, "", {remote.IPAddress, remote.port, remote.displayName}});
	};

	// Both peers ready -> init messaging
	svcCB.onConnected = [this](ISession::pointer session)
	{
		pImpl->communication.init(session, pImpl->config.secret);
		pImpl->communication.start();
		pImpl->connectionState = ConnectionState::Connected;

		if (pImpl->callbacks.onConnectionChanged)
			pImpl->callbacks.onConnectionChanged({ConnectionState::Connected, "", {}});
	};

	svcCB.onConnectionFailed = [this](const std::string &reason)
	{
		pImpl->connectionState = ConnectionState::Error;
		if (pImpl->callbacks.onConnectionChanged)
			pImpl->callbacks.onConnectionChanged({ConnectionState::Error, reason, {}});
	};

	svcCB.onDisconnected = [this]()
	{
		pImpl->communication.deinit();
		pImpl->connectionState = ConnectionState::Disconnected;
		if (pImpl->callbacks.onConnectionChanged)
			pImpl->callbacks.onConnectionChanged({ConnectionState::Disconnected, "", {}});
	};

	pImpl->connectionService.setCallbacks(std::move(svcCB));

	// Set callback for when a remote peer was found
	pImpl->discovery.setOnRemoteFound(
		[this](const DiscoveryEndpoint &ep)
		{
			pImpl->signaling.registerPeer(ep.displayName, ep.IPAddress, ep.signalingPort);
			pImpl->validation.onPeerDiscovered(ep);
		});

	// Wire PeerValidationSendCallbacks
	PeerValidationSendCallbacks sendCb;
	sendCb.sendRequest		   = [this](const std::string &name, RemoteRequest req) { pImpl->signaling.sendValidationRequest(name, req); };
	sendCb.sendSecretResponse  = [this](const std::string &name, const std::string &v) { pImpl->signaling.sendSecretResponse(name, v); };
	sendCb.sendVersionResponse = [this](const std::string &name, const std::string &v) { pImpl->signaling.sendVersionResponse(name, v); };
	sendCb.sendHandshake	   = [this](const std::string &name) { pImpl->signaling.sendValidationHandshake(name); };
	pImpl->validation.setSendCallbacks(std::move(sendCb));
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
	pImpl->connectionService.requestConnection(ep);

	pImpl->connectionState = ConnectionState::Searching;

	return true;
}


void netlink::NetLink::respondToConnection(bool accepted)
{
	pImpl->connectionService.respondToConnection(accepted);
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
