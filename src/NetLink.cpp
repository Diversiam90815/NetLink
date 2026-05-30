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

	svcCB.onStatusUpdate = [this](const ConnectionStatusUpdate &update)
	{
		switch (update.type)
		{
		case ConnectionStatusUpdate::Type::Established:
			// Wire the live session into the messaging layer and tell the app.
			pImpl->communication.init(update.session);
			pImpl->communication.start();
			pImpl->connectionState = ConnectionState::Connected;
			if (pImpl->callbacks.onConnectionChanged)
				pImpl->callbacks.onConnectionChanged({ConnectionState::Connected, "", {}});
			break;

		case ConnectionStatusUpdate::Type::InvitationReceived:
			// Remote wants to connect — surface to app so it can call respondToConnection().
			pImpl->connectionState = ConnectionState::PendingInbound;
			if (pImpl->callbacks.onConnectionChanged)
			{
				const auto &ep = update.endpoint;
				pImpl->callbacks.onConnectionChanged({ConnectionState::PendingInbound, "", {ep.IPAddress, ep.port, ep.displayName}});
			}
			break;

		case ConnectionStatusUpdate::Type::Failed:
		case ConnectionStatusUpdate::Type::Declined:
			pImpl->connectionState = ConnectionState::Error;
			if (pImpl->callbacks.onConnectionChanged)
				pImpl->callbacks.onConnectionChanged({ConnectionState::Error, update.message, {}});
			break;

		case ConnectionStatusUpdate::Type::Closed:
			// Covers both local-initiated and remote-initiated disconnects.
			pImpl->communication.deinit();
			pImpl->connectionState = ConnectionState::Disconnected;
			if (pImpl->callbacks.onConnectionChanged)
				pImpl->callbacks.onConnectionChanged({ConnectionState::Disconnected, update.message, {}});
			break;

		case ConnectionStatusUpdate::Type::Initiated:
		case ConnectionStatusUpdate::Type::InvitationSent:
		case ConnectionStatusUpdate::Type::Accepted:
		case ConnectionStatusUpdate::Type::Establishing:
		case ConnectionStatusUpdate::Type::Closing:
			// In-progress transitions — no public state change yet.
			break;
		}
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
	pImpl->validation.setLocalSecret(pImpl->config.secret);

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

	// When signaling socket binds (on init or adapter change), update discovery with the new port
	pImpl->signaling.setOnSocketBound(
		[this](int boundPort)
		{
			DiscoveryConfig cfg = pImpl->discovery.getConfig(); // see note below
			cfg.signalingPort	= boundPort;
			pImpl->discovery.init(cfg);							// no rebind — only signalingPort changed
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


std::vector<netlink::Endpoint> netlink::NetLink::getPotentialEndpoints()
{
	auto				  validated = pImpl->validation.getValidatedPeers();

	std::vector<Endpoint> result;
	result.reserve(validated.size());

	for (const auto &vr : validated)
	{
		result.push_back({vr.remoteEndpoint.IPAddress, vr.remoteEndpoint.port, vr.remoteEndpoint.displayName});
	}

	return result;
}


bool netlink::NetLink::connectTo(const Endpoint &remote)
{
	return pImpl->connectionService.initiateConnection(remote.displayName);
}


void netlink::NetLink::respondToConnection(bool accepted)
{
	auto remoteOpt = pImpl->connectionService.getCurrentRemote();
	if (!remoteOpt.has_value())
		return;

	const auto &name = remoteOpt->displayName;

	if (accepted)
		pImpl->connectionService.acceptIncomingConnection(name);
	else
		pImpl->connectionService.declineIncomingConnection(name, "User declined");
}


void netlink::NetLink::disconnect()
{
	auto remoteOpt = pImpl->connectionService.getCurrentRemote();
	if (remoteOpt.has_value())
		pImpl->connectionService.closeConnection(remoteOpt->displayName);
	// communication.deinit() and state update are handled by onStatusUpdate(Closed)
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


int netlink::NetLink::getActiveAdapterID() const
{
	return pImpl->network.getCurrentNetworkAdapter().ID;
}
