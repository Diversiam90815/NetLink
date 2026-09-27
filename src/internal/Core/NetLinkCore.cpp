/*
  ==============================================================================
	Module:         NetLinkCore
	Description:    Composition root of the library: owns every service, wires
					them together and delivers public events on a dedicated
					event thread.
  ==============================================================================
*/

#include "NetLinkCore.h"

#include "NetLinkLog.h"
#include "NetLinkVersion.h"


netlink::NetLinkCore::NetLinkCore(const NetLinkCoreDependencies &dependencies)
	: mDiscovery(dependencies.datagramSocketFactory), mChannel(dependencies.datagramSocketFactory, dependencies.channelConfig), mChannelConfig(dependencies.channelConfig),
	  mConnectionService(mChannel)
{
	mEvents.start();
	wireServices();
}


netlink::NetLinkCore::~NetLinkCore()
{
	shutdown();
	mEvents.stop();
}


// ---------------------------------------------------------------------------
// Wiring: the only place where services get to know each other
// ---------------------------------------------------------------------------

void netlink::NetLinkCore::wireServices()
{
	// Discovery -> channel registry + validation handshake
	mDiscovery.setOnRemoteFound(
		[this](const DiscoveryEndpoint &endpoint)
		{
			mChannel.registerPeer(endpoint.displayName, endpoint.IPAddress, endpoint.port);

			// New or changed endpoint (e.g. the remote restarted): validate again
			if (mValidation.getValidationResult(endpoint.displayName).has_value())
				mValidation.clearValidatedPeer(endpoint.displayName);

			mValidation.onPeerDiscovered(endpoint);
		});

	// Discovery -> forget a peer that left the network
	mDiscovery.setOnRemoteLost(
		[this](const DiscoveryEndpoint &endpoint)
		{
			// The peer of an active session may legitimately stop announcing; dropping
			// its channel registration here would break that connection
			if (const auto current = mConnectionService.getCurrentRemote(); current.has_value() && current->displayName == endpoint.displayName)
				return;

			mChannel.unregisterPeer(endpoint.displayName);
			mValidation.clearValidatedPeer(endpoint.displayName);

			postEvent(
				[peer = toPublicEndpoint(endpoint)](const NetLinkCallbacks &callbacks)
				{
					if (callbacks.onRemoteLost)
						callbacks.onRemoteLost(peer);
				});
		});

	// Channel -> connection lifecycle
	ChannelConnectionCallbacks connectionSignals;
	connectionSignals.onConnectRequested	   = [this](const std::string &name) { mConnectionService.onReceivedInvitation(name); };
	connectionSignals.onConnectRequestAnswered = [this](const std::string &name, const bool accepted, const std::string &reason)
	{ mConnectionService.onReceivedAnswerToInvite(name, accepted, reason); };
	connectionSignals.onDisconnectReceived = [this](const std::string &name) { mConnectionService.onDisconnectReceived(name); };
	connectionSignals.onReadyFlagReceived  = [this](const std::string &name) { mConnectionService.onReadyFlagReceived(name); };
	mChannel.setConnectionCallbacks(std::move(connectionSignals));

	// Channel -> peer validation
	ChannelValidationCallbacks validationSignals;
	validationSignals.onValidationRequestReceived = [this](const std::string &name, const RemoteRequest request) { mValidation.onRequestReceived(name, request); };
	validationSignals.onSecretResponseReceived	  = [this](const std::string &name, const std::string &secret)
	{ mValidation.onCheckResponseReceived(name, RemoteRequest::Secret, secret); };
	validationSignals.onVersionResponseReceived = [this](const std::string &name, const std::string &version)
	{ mValidation.onCheckResponseReceived(name, RemoteRequest::Version, version); };
	validationSignals.onValidationHandshakeReceived = [this](const std::string &name) { mValidation.onHandshakeReceived(name); };
	mChannel.setValidationCallbacks(std::move(validationSignals));

	// Peer validation -> channel (outgoing) and -> connection service (results)
	PeerValidationSendCallbacks validationSend;
	validationSend.sendRequest		   = [this](const std::string &name, const RemoteRequest request) { mChannel.sendValidationRequest(name, request); };
	validationSend.sendSecretResponse  = [this](const std::string &name, const std::string &value) { mChannel.sendSecretResponse(name, value); };
	validationSend.sendVersionResponse = [this](const std::string &name, const std::string &value) { mChannel.sendVersionResponse(name, value); };
	validationSend.sendHandshake	   = [this](const std::string &name) { mChannel.sendValidationHandshake(name); };
	mValidation.setSendCallbacks(std::move(validationSend));
	mValidation.setValidationCallback([this](const ValidationResult &result) { onValidationResult(result); });

	// Connection lifecycle -> keepalive + public events
	ConnectionServiceCallbacks connectionCallbacks;
	connectionCallbacks.onStatusUpdate = [this](const ConnectionStatusUpdate &update) { onConnectionStatus(update); };
	mConnectionService.setCallbacks(std::move(connectionCallbacks));

	// Channel -> public events: only messages of the connected remote reach the application
	mChannel.setMessageCallback(
		[this](const std::string &name, const uint32_t type, std::vector<uint8_t> data)
		{
			if (mState.load() != ConnectionState::Connected)
				return;

			if (const auto remote = mConnectionService.getCurrentRemote(); !remote.has_value() || remote->displayName != name)
				return;

			postEvent(
				[message = Message{.type = type, .data = std::move(data)}](const NetLinkCallbacks &callbacks)
				{
					if (callbacks.onMessageReceived)
						callbacks.onMessageReceived(message);
				});
		});

	// Channel -> connection loss (unacknowledged messages, silence, peer restart)
	mChannel.setOnPeerLost([this](const std::string &name, const std::string &reason) { mConnectionService.onPeerLost(name, reason); });
}


// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void netlink::NetLinkCore::configure(const NetLinkConfig &config, const NetLinkCallbacks &callbacks)
{
	{
		std::lock_guard<std::mutex> lock(mConfigMutex);
		mConfig = config;
	}

	{
		std::lock_guard<std::mutex> lock(mCallbacksMutex);
		mCallbacks = std::make_shared<const NetLinkCallbacks>(callbacks);
	}

	PeerChannelConfig channelConfig;
	{
		std::lock_guard<std::mutex> lock(mConfigMutex);
		mChannelConfig.reliability.sendQueueCapacity = config.sendQueueCapacity;
		mChannelConfig.reliability.sendQueueOverflow = config.sendQueueOverflow;
		channelConfig								 = mChannelConfig;
	}
	mChannel.setConfig(channelConfig);

	// A configured secret must match on both sides; an empty secret disables the check
	PeerValidationConfig validationConfig;
	validationConfig.enableSecretCheck	= !config.secret.empty();
	validationConfig.enableVersionCheck = true;
	mValidation.setConfig(validationConfig);
	mValidation.setLocalSecret(config.secret);

	const std::string version = config.applicationVersion.empty() ? std::string{internal::Version} : config.applicationVersion;
	mValidation.setLocalVersion(version);

	{
		std::lock_guard<std::mutex> lock(mConfigMutex);
		mLocalVersion = version;
	}
}


bool netlink::NetLinkCore::init()
{
	std::string displayName;
	std::string version;
	{
		std::lock_guard<std::mutex> lock(mConfigMutex);
		displayName = mConfig.localDisplayName;
		version		= mLocalVersion;
	}

	if (!mChannel.init(displayName))
	{
		NETLINK_LOG_ERROR("NetLink init failed: a local display name is required");
		return false;
	}

	NETLINK_LOG_INFO("NetLink {} starting as '{}', advertising protocol version {}", internal::Version, displayName, version.empty() ? std::string{internal::Version} : version);

	mInitialized.store(true);
	applyLocalAddress();
	return true;
}


void netlink::NetLinkCore::shutdown()
{
	// Tell the remote we are leaving while the channel is still available, and give the Disconnect a moment to be acknowledged
	if (const auto remote = mConnectionService.getCurrentRemote(); remote.has_value())
	{
		mConnectionService.closeConnection(remote->displayName);
		mChannel.flush(remote->displayName, ShutdownFlushTimeout);
	}

	mValidation.cancelAllPendingValidation();
	mDiscovery.deinit();
	mChannel.deinit();

	mInitialized.store(false);
	mState.store(ConnectionState::None);
}


void netlink::NetLinkCore::setLocalAddress(const std::string &ipv4, const std::string &subnetMask)
{
	// Boundary between the OS-facing string addresses and the validated type used internally
	const auto parsed = net::IPv4Address::parse(ipv4);

	if (!parsed.has_value())
	{
		NETLINK_LOG_ERROR("Ignoring malformed local address '{}'", ipv4);
		return;
	}

	// An unusable mask simply disables subnet scoping rather than failing the switch
	{
		const auto					mask = net::IPv4Address::parse(subnetMask);
		std::lock_guard<std::mutex> lock(mConfigMutex);
		mLocalAddress = *parsed;
		mSubnetMask	  = mask.value_or(net::IPv4Address{});
	}

	if (mInitialized.load())
		applyLocalAddress();
}


void netlink::NetLinkCore::applyLocalAddress()
{
	net::IPv4Address address;
	{
		std::lock_guard<std::mutex> lock(mConfigMutex);
		address = mLocalAddress;
	}

	if (address.isUnspecified())
		return;

	mConnectionService.setLocalIP(address);
	mChannel.setLocalIPv4(address);
	updateDiscoveryConfig();
}


void netlink::NetLinkCore::updateDiscoveryConfig()
{
	DiscoveryConfig discoveryConfig;

	{
		std::lock_guard<std::mutex> lock(mConfigMutex);
		discoveryConfig.localIPv4		 = mLocalAddress;
		discoveryConfig.subnetMask		 = mSubnetMask;
		discoveryConfig.displayName		 = mConfig.localDisplayName;
		discoveryConfig.discoveryPort	 = mConfig.discoveryPort;
		// A malformed configured broadcast address falls back to the global one
		discoveryConfig.broadcastAddress = net::IPv4Address::parse(mConfig.broadcastAddress).value_or(net::IPv4Address::broadcast());
	}

	discoveryConfig.channelPort = mChannel.getBoundPort();

	if (!mDiscovery.init(discoveryConfig))
		NETLINK_LOG_ERROR("Discovery could not be configured for {}", discoveryConfig.localIPv4.toString());
}


// ---------------------------------------------------------------------------
// Discovery & connection
// ---------------------------------------------------------------------------

bool netlink::NetLinkCore::startDiscovery()
{
	if (!mDiscovery.startDiscovery())
	{
		NETLINK_LOG_ERROR("Cannot start discovery: no local address selected?");
		return false;
	}

	mChannel.start();

	auto expected = ConnectionState::None;
	mState.compare_exchange_strong(expected, ConnectionState::Searching);
	return true;
}


void netlink::NetLinkCore::stopDiscovery()
{
	// The channel keeps running: established peers still need it for their session
	mDiscovery.stopDiscovery();

	auto expected = ConnectionState::Searching;
	mState.compare_exchange_strong(expected, ConnectionState::None);
}


std::vector<netlink::Endpoint> netlink::NetLinkCore::getPotentialEndpoints() const
{
	std::vector<Endpoint> result;

	for (const auto &validated : mValidation.getValidatedPeers())
		result.push_back(toPublicEndpoint(validated.remoteEndpoint));

	return result;
}


bool netlink::NetLinkCore::connectTo(const Endpoint &remote)
{
	return mConnectionService.initiateConnection(remote.displayName);
}


void netlink::NetLinkCore::respondToConnection(const bool accepted)
{
	const auto remote = mConnectionService.getCurrentRemote();
	if (!remote.has_value())
		return;

	if (accepted)
		mConnectionService.acceptIncomingConnection(remote->displayName);
	else
		mConnectionService.declineIncomingConnection(remote->displayName, "User declined");
}


void netlink::NetLinkCore::disconnect()
{
	if (const auto remote = mConnectionService.getCurrentRemote(); remote.has_value())
		mConnectionService.closeConnection(remote->displayName);
}


bool netlink::NetLinkCore::send(const uint32_t type, const std::vector<uint8_t> &payload, const DeliveryMode mode)
{
	if (mState.load() != ConnectionState::Connected)
		return false;

	const auto remote = mConnectionService.getCurrentRemote();
	if (!remote.has_value())
		return false;

	return mChannel.sendMessage(remote->displayName, type, payload, mode);
}


void netlink::NetLinkCore::postEvent(Event event)
{
	mEvents.post(
		[this, event = std::move(event)]()
		{
			std::shared_ptr<const NetLinkCallbacks> callbacks;
			{
				std::lock_guard<std::mutex> lock(mCallbacksMutex);
				callbacks = mCallbacks;
			}

			if (callbacks)
				event(*callbacks);
		});
}


// ---------------------------------------------------------------------------
// Internal events
// ---------------------------------------------------------------------------

void netlink::NetLinkCore::onValidationResult(const ValidationResult &result)
{
	mConnectionService.onPeerValidated(result);

	if (!result.canConnect)
	{
		NETLINK_LOG_WARNING("Peer {} is not compatible: {}", result.remoteEndpoint.displayName, result.message);
		return;
	}

	postEvent(
		[endpoint = toPublicEndpoint(result.remoteEndpoint)](const NetLinkCallbacks &callbacks)
		{
			if (callbacks.onRemoteDiscovered)
				callbacks.onRemoteDiscovered(endpoint);
		});
}


void netlink::NetLinkCore::onConnectionStatus(const ConnectionStatusUpdate &update)
{
	// Runs while ConnectionService holds its lock: only internal bookkeeping here, app code goes through postEvent()
	switch (update.type)
	{
	case ConnectionStatusUpdate::Type::Established:
		mChannel.setKeepAlive(update.endpoint.displayName, true);
		mState.store(ConnectionState::Connected);
		emitConnectionChanged(ConnectionState::Connected, update.message, update.endpoint);
		break;

	case ConnectionStatusUpdate::Type::InvitationReceived:
		mState.store(ConnectionState::PendingInbound);
		emitConnectionChanged(ConnectionState::PendingInbound, update.message, update.endpoint);
		break;

	case ConnectionStatusUpdate::Type::Failed:
	case ConnectionStatusUpdate::Type::Declined:
		endSessionTraffic(update.endpoint.displayName);
		mState.store(ConnectionState::Error);
		emitConnectionChanged(ConnectionState::Error, update.message, update.endpoint);
		break;

	case ConnectionStatusUpdate::Type::Closed:
		endSessionTraffic(update.endpoint.displayName);
		mState.store(ConnectionState::Disconnected);
		emitConnectionChanged(ConnectionState::Disconnected, update.message, update.endpoint);
		break;

	case ConnectionStatusUpdate::Type::Initiated:
	case ConnectionStatusUpdate::Type::InvitationSent:
	case ConnectionStatusUpdate::Type::Accepted:
	case ConnectionStatusUpdate::Type::Establishing:
	case ConnectionStatusUpdate::Type::Closing: break; // In-progress transitions, no public state change
	}
}


void netlink::NetLinkCore::endSessionTraffic(const std::string &remote)
{
	if (remote.empty())
		return;

	// Control signals (e.g. the Disconnect) still go out, unsent application data of the ended session does not
	mChannel.setKeepAlive(remote, false);
	mChannel.dropApplicationTraffic(remote);
}


void netlink::NetLinkCore::emitConnectionChanged(const ConnectionState state, const std::string &message, const DiscoveryEndpoint &remote)
{
	postEvent(
		[event = ConnectionEvent{.state = state, .errorMessage = message, .remote = toPublicEndpoint(remote)}](const NetLinkCallbacks &callbacks)
		{
			if (callbacks.onConnectionChanged)
				callbacks.onConnectionChanged(event);
		});
}


netlink::Endpoint netlink::NetLinkCore::toPublicEndpoint(const DiscoveryEndpoint &endpoint)
{
	return {.IPAddress = endpoint.IPAddress.toString(), .port = endpoint.port, .displayName = endpoint.displayName};
}
