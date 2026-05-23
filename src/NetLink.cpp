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
#include "Network/NetworkInformation.h"


struct netlink::NetLink::Impl
{
	NetLinkConfig				   config;
	NetLinkCallbacks			   callbacks;

	asio::io_context			   ioContext;
	DiscoveryService			   discovery{ioContext};
	SignalingService			   signaling{ioContext};
	netlink::NetworkInformation	   network;
	netlink::TCPTransportFactory   transportFactory;
	netlink::PeerValidationService validation;
	netlink::ConnectionService	   connectionService{ioContext, signaling, transportFactory};
	RemoteCommunication			   communication;

	ConnectionState				   connectionState{ConnectionState::None};
};


// ---------------------------------------------------------------------------
// Helpers — map internal <-> public types
// ---------------------------------------------------------------------------

static netlink::AdapterPriority mapPriority(netlink::AdapterPriorityInternal internal)
{
	switch (internal)
	{
	case netlink::AdapterPriorityInternal::Preferred: return netlink::AdapterPriority::Preferred;
	case netlink::AdapterPriorityInternal::Available: return netlink::AdapterPriority::Available;
	default: return netlink::AdapterPriority::Suppressed;
	}
}

static netlink::NetworkAdapter toPublicAdapter(const netlink::NetworkAdapterInternal &internal)
{
	netlink::NetworkAdapter pub;
	pub.adapterName = internal.AdapterName;
	pub.networkName = internal.NetworkName;
	pub.ipv4		= internal.IPv4;
	pub.id			= internal.ID;
	pub.priority	= mapPriority(internal.Priority);
	return pub;
}


// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

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
			pImpl->signaling.registerPeer(ep.displayName, ep.IPAddress, ep.port);
			pImpl->validation.onPeerDiscovered(ep);
		});
}


bool netlink::NetLink::init()
{
	// Initialize services and setup internal callbacks
	
	// Wire PeerValidationSendCallbacks
	PeerValidationSendCallbacks sendCb;
	sendCb.sendRequest		   = [this](const std::string &name, RemoteRequest req) { pImpl->signaling.sendValidationRequest(name, req); };
	sendCb.sendSecretResponse  = [this](const std::string &name, const std::string &v) { pImpl->signaling.sendSecretResponse(name, v); };
	sendCb.sendVersionResponse = [this](const std::string &name, const std::string &v) { pImpl->signaling.sendVersionResponse(name, v); };
	sendCb.sendHandshake	   = [this](const std::string &name) { pImpl->signaling.sendValidationHandshake(name); };
	pImpl->validation.setSendCallbacks(std::move(sendCb));

	// Network adapter changed callback
	pImpl->network.setOnAdapterChanged(
		[this](const std::string &newIPv4)
		{
			pImpl->signaling.setLocalIPv4(newIPv4);
			pImpl->connectionService.setLocalIP(newIPv4);

			DiscoveryConfig disConf;
			disConf.localIPv4		 = newIPv4;
			disConf.displayName		 = pImpl->config.localDisplayName;
			disConf.discoveryPort	 = pImpl->config.discoveryPort;
			disConf.broadCastAddress = pImpl->config.broadcastAddress;
			pImpl->discovery.init(disConf);
		});

	if (!pImpl->network.init())
		return false;

	pImpl->network.processAdapter();

	if (!pImpl->signaling.init(pImpl->config.localDisplayName))
		return false;

	// Apply the current adapter immediately if one is already set
	const auto &adapter = pImpl->network.getCurrentNetworkAdapter();
	if (adapter.isValid())
	{
		pImpl->signaling.setLocalIPv4(adapter.IPv4);
		pImpl->connectionService.setLocalIP(adapter.IPv4);
	}

	return true;
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
	return true;
}


void					 netlink::NetLink::respondToConnection(bool accepted) {}


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
	const auto				   &internal = pImpl->network.getAvailableNetworkAdapters();

	std::vector<NetworkAdapter> result;
	result.reserve(internal.size());

	for (const auto &a : internal)
		result.push_back(toPublicAdapter(a));

	return result;
}


bool netlink::NetLink::setActiveAdapter(const int &adapterID)
{
	// setCurrentNetworkAdapter fires onAdapterChanged internally
	return pImpl->network.setCurrentNetworkAdapter(adapterID);
}
