/*
==============================================================================
	Module:         NetLink
	Description:    API for NetLink library
  ==============================================================================
*/

#include "NetLink/NetLink.h"

#include <atomic>

#include "Discovery/DiscoveryService.h"
#include "Signaling/SignalingService.h"
#include "Transport/TransportInterfaces.h"
#include "Transport/TransportFactory.h"
#include "Messaging/RemoteCommunication.h"
#include "ConnectionService/ConnectionService.h"
#include "PeerValidation/PeerValidationService.h"
#include "Network/NetworkInformation.h"


struct netlink::NetLink::Impl
{
	NetLinkConfig					   config;
	NetLinkCallbacks				   callbacks;

	DiscoveryService				   discovery{};
	SignalingService				   signaling{};
	netlink::NetworkInformation		   network;
	TransportKind					   transportKind{TransportKind::Tcp};
	std::unique_ptr<ITransportFactory> transportFactory{createTransportFactory(transportKind)};
	netlink::PeerValidationService	   validation;
	netlink::ConnectionService		   connectionService{signaling, *transportFactory};
	RemoteCommunication				   communication;

	std::atomic<ConnectionState>	   connectionState{ConnectionState::None};

	void							   updateDiscoveryConfig(const std::string &localIPv4)
	{
		DiscoveryConfig disConf	 = discovery.getConfig();
		disConf.localIPv4		 = localIPv4;
		disConf.displayName		 = config.localDisplayName;
		disConf.discoveryPort	 = config.discoveryPort;
		disConf.broadcastAddress = config.broadcastAddress;
		disConf.signalingPort	 = signaling.getBoundPort();
		discovery.init(disConf);
	}
};


// ---------------------------------------------------------------------------
// Helpers: map internal <-> public types
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


netlink::NetLink &netlink::NetLink::operator=(NetLink &&other) noexcept
{
	if (this != &other)
	{
		shutdown();
		pImpl = std::move(other.pImpl);
	}
	return *this;
}


void netlink::NetLink::configure(const NetLinkConfig &config, const NetLinkCallbacks &callbacks)
{
	Impl *impl		= pImpl.get();

	impl->config	= config;
	impl->callbacks = callbacks;

	if (config.transport != impl->transportKind)
	{
		impl->transportKind	   = config.transport;
		impl->transportFactory = createTransportFactory(config.transport);
		impl->connectionService.setTransportFactory(*impl->transportFactory);
	}

	impl->communication.setMessageCallback(
		[impl](uint32_t type, std::vector<uint8_t> &data)
		{
			if (impl->callbacks.onMessageReceived)
				impl->callbacks.onMessageReceived(Message{type, std::move(data)});
		});

	impl->communication.setDisconnectedCallback([impl](const std::string &reason) { impl->connectionService.onTransportDisconnected(reason); });

	netlink::ConnectionServiceCallbacks svcCB;

	svcCB.onStatusUpdate = [impl](const ConnectionStatusUpdate &update)
	{
		switch (update.type)
		{
		case ConnectionStatusUpdate::Type::Established:
			// Wire the live session into the messaging layer and tell the app.
			impl->communication.init(update.session);
			impl->communication.start();
			impl->connectionState = ConnectionState::Connected;
			if (impl->callbacks.onConnectionChanged)
				impl->callbacks.onConnectionChanged({ConnectionState::Connected, "", {}});
			break;

		case ConnectionStatusUpdate::Type::InvitationReceived:
			// Remote wants to connect — surface to app so it can call respondToConnection().
			impl->connectionState = ConnectionState::PendingInbound;
			if (impl->callbacks.onConnectionChanged)
			{
				const auto &ep = update.endpoint;
				impl->callbacks.onConnectionChanged({ConnectionState::PendingInbound, "", {ep.IPAddress, ep.port, ep.displayName}});
			}
			break;

		case ConnectionStatusUpdate::Type::Failed:
		case ConnectionStatusUpdate::Type::Declined:
			impl->connectionState = ConnectionState::Error;
			if (impl->callbacks.onConnectionChanged)
				impl->callbacks.onConnectionChanged({ConnectionState::Error, update.message, {}});
			break;

		case ConnectionStatusUpdate::Type::Closed:
			// Covers local disconnects, remote disconnects and lost transports.
			impl->communication.deinit();
			impl->connectionState = ConnectionState::Disconnected;
			if (impl->callbacks.onConnectionChanged)
				impl->callbacks.onConnectionChanged({ConnectionState::Disconnected, update.message, {}});
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

	impl->connectionService.setCallbacks(std::move(svcCB));

	// Set callback for when a remote peer was found
	impl->discovery.setOnRemoteFound(
		[impl](const DiscoveryEndpoint &ep)
		{
			impl->signaling.registerPeer(ep.displayName, ep.IPAddress, ep.port);
			impl->validation.onPeerDiscovered(ep);

			if (impl->callbacks.onRemoteDiscovered)
				impl->callbacks.onRemoteDiscovered({ep.IPAddress, ep.port, ep.displayName});
		});
}


bool netlink::NetLink::init()
{
	Impl					   *impl = pImpl.get();

	// Wire PeerValidationSendCallbacks
	PeerValidationSendCallbacks sendCb;
	sendCb.sendRequest		   = [impl](const std::string &name, RemoteRequest req) { impl->signaling.sendValidationRequest(name, req); };
	sendCb.sendSecretResponse  = [impl](const std::string &name, const std::string &v) { impl->signaling.sendSecretResponse(name, v); };
	sendCb.sendVersionResponse = [impl](const std::string &name, const std::string &v) { impl->signaling.sendVersionResponse(name, v); };
	sendCb.sendHandshake	   = [impl](const std::string &name) { impl->signaling.sendValidationHandshake(name); };
	impl->validation.setSendCallbacks(std::move(sendCb));
	impl->validation.setLocalSecret(impl->config.secret);

	// Network adapter changed: rebind signaling (announces the new port via onSocketBound) and move discovery to the new address
	impl->network.setOnAdapterChanged(
		[impl](const std::string &newIPv4)
		{
			impl->connectionService.setLocalIP(newIPv4);
			impl->signaling.setLocalIPv4(newIPv4);
			impl->updateDiscoveryConfig(newIPv4);
		});

	// When the signaling socket binds, announce the new port through discovery
	impl->signaling.setOnSocketBound(
		[impl](int boundPort)
		{
			DiscoveryConfig cfg = impl->discovery.getConfig();
			cfg.signalingPort	= boundPort;
			impl->discovery.init(cfg); // no rebind — only the announced port changed
		});

	if (!impl->network.init())
		return false;

	impl->network.processAdapter();

	if (!impl->signaling.init(impl->config.localDisplayName))
		return false;

	// Apply the current adapter immediately if one is already set
	const auto &adapter = impl->network.getCurrentNetworkAdapter();
	if (adapter.isValid())
	{
		impl->connectionService.setLocalIP(adapter.IPv4);
		impl->signaling.setLocalIPv4(adapter.IPv4);
		impl->updateDiscoveryConfig(adapter.IPv4);
	}

	return true;
}


void netlink::NetLink::shutdown()
{
	if (!pImpl)
		return;

	pImpl->communication.deinit();
	pImpl->signaling.deinit();
	pImpl->discovery.deinit();
	pImpl->connectionState = ConnectionState::None;
}


bool netlink::NetLink::startDiscovery()
{
	try
	{
		pImpl->discovery.startDiscovery();
	}
	catch (const std::exception &)
	{
		return false; // not initialized / no adapter selected
	}

	pImpl->signaling.start();
	pImpl->connectionState = ConnectionState::Searching;
	return true;
}


void netlink::NetLink::stopDiscovery()
{
	pImpl->discovery.stopDiscovery();
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
}


netlink::ConnectionState netlink::NetLink::getConnectionState() const
{
	return pImpl->connectionState.load();
}


bool netlink::NetLink::send(const Message &message, DeliveryMode mode)
{
	return send(message.type, message.data, mode);
}


bool netlink::NetLink::send(uint32_t type, const std::vector<uint8_t> &payload, DeliveryMode mode)
{
	if (pImpl->connectionState.load() != ConnectionState::Connected)
		return false;

	pImpl->communication.write(type, payload, mode);
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
	return pImpl->network.setCurrentNetworkAdapter(adapterID);
}


int netlink::NetLink::getActiveAdapterID() const
{
	return pImpl->network.getCurrentNetworkAdapter().ID;
}
