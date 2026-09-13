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


netlink::NetLinkCore::NetLinkCore(NetLinkCoreDependencies dependencies)
	: mDiscovery(dependencies.datagramSocketFactory), mSignaling(dependencies.datagramSocketFactory), mTransportFactory(createTransportFactory(mTransportKind)),
	  mConnectionService(mSignaling, *mTransportFactory)
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
	// Discovery -> signaling registry + validation handshake
	mDiscovery.setOnRemoteFound(
		[this](const DiscoveryEndpoint &endpoint)
		{
			mSignaling.registerPeer(endpoint.displayName, endpoint.IPAddress, endpoint.port);

			// New or changed endpoint (e.g. the remote restarted): validate again
			if (mValidation.getValidationResult(endpoint.displayName).has_value())
				mValidation.clearValidatedPeer(endpoint.displayName);

			mValidation.onPeerDiscovered(endpoint);
		});

	// Signaling -> connection lifecycle
	SignalingConnectionCallbacks connectionSignals;
	connectionSignals.onConnectRequested	   = [this](const std::string &name) { mConnectionService.onReceivedInvitation(name); };
	connectionSignals.onConnectRequestAnswered = [this](const std::string &name, bool accepted) { mConnectionService.onReceivedAnswerToInvite(name, accepted, ""); };
	connectionSignals.onDisconnectReceived	   = [this](const std::string &name) { mConnectionService.onDisconnectReceived(name); };
	connectionSignals.onReadyFlagReceived	   = [this](const std::string &name) { mConnectionService.onReadyFlagReceived(name); };
	connectionSignals.onDataPortReceived	   = [this](const std::string &name, int port) { mConnectionService.onDataPortReceived(name, port); };
	mSignaling.setConnectionCallbacks(std::move(connectionSignals));

	// Signaling -> peer validation
	SignalingValidationCallbacks validationSignals;
	validationSignals.onValidationRequestReceived	= [this](const std::string &name, RemoteRequest request) { mValidation.onRequestReceived(name, request); };
	validationSignals.onSecretResponseReceived		= [this](const std::string &name, const std::string &secret) { mValidation.onCheckResponseReceived(name, RemoteRequest::Secret, secret); };
	validationSignals.onVersionResponseReceived		= [this](const std::string &name, const std::string &version) { mValidation.onCheckResponseReceived(name, RemoteRequest::Version, version); };
	validationSignals.onValidationHandshakeReceived = [this](const std::string &name) { mValidation.onHandshakeReceived(name); };
	mSignaling.setValidationCallbacks(std::move(validationSignals));

	// Peer validation -> signaling (outgoing) and -> connection service (results)
	PeerValidationSendCallbacks validationSend;
	validationSend.sendRequest		   = [this](const std::string &name, RemoteRequest request) { mSignaling.sendValidationRequest(name, request); };
	validationSend.sendSecretResponse  = [this](const std::string &name, const std::string &value) { mSignaling.sendSecretResponse(name, value); };
	validationSend.sendVersionResponse = [this](const std::string &name, const std::string &value) { mSignaling.sendVersionResponse(name, value); };
	validationSend.sendHandshake	   = [this](const std::string &name) { mSignaling.sendValidationHandshake(name); };
	mValidation.setSendCallbacks(std::move(validationSend));
	mValidation.setValidationCallback([this](const ValidationResult &result) { onValidationResult(result); });

	// Connection lifecycle -> messaging + public events
	ConnectionServiceCallbacks connectionCallbacks;
	connectionCallbacks.onStatusUpdate = [this](const ConnectionStatusUpdate &update) { onConnectionStatus(update); };
	mConnectionService.setCallbacks(std::move(connectionCallbacks));

	// Messaging -> public events / connection loss
	mCommunication.setMessageCallback(
		[this](uint32_t type, std::vector<uint8_t> &data)
		{
			postEvent([message = Message{type, std::move(data)}](const NetLinkCallbacks &callbacks)
					  {
						  if (callbacks.onMessageReceived)
							  callbacks.onMessageReceived(message);
					  });
		});

	mCommunication.setDisconnectedCallback([this](const std::string &reason) { mConnectionService.onTransportDisconnected(reason); });
}


// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void netlink::NetLinkCore::configure(const NetLinkConfig &config, const NetLinkCallbacks &callbacks)
{
	mConfig	   = config;
	mCallbacks = callbacks;

	if (config.transport != mTransportKind)
	{
		mTransportKind	  = config.transport;
		mTransportFactory = createTransportFactory(config.transport);
		mConnectionService.setTransportFactory(*mTransportFactory);
	}

	// A configured secret must match on both sides; an empty secret disables the check
	PeerValidationConfig validationConfig;
	validationConfig.enableSecretCheck = !config.secret.empty();
	mValidation.setConfig(validationConfig);
	mValidation.setLocalSecret(config.secret);
}


bool netlink::NetLinkCore::init()
{
	if (!mSignaling.init(mConfig.localDisplayName))
	{
		NETLINK_LOG_ERROR("NetLink init failed: a local display name is required");
		return false;
	}

	mInitialized.store(true);
	applyLocalAddress();
	return true;
}


void netlink::NetLinkCore::shutdown()
{
	// Tell the remote we are leaving while signaling is still available
	if (auto remote = mConnectionService.getCurrentRemote(); remote.has_value())
		mConnectionService.closeConnection(remote->displayName);

	mCommunication.deinit();
	mValidation.cancelAllPendingValidation();
	mDiscovery.deinit();
	mSignaling.deinit();

	mInitialized.store(false);
	mState.store(ConnectionState::None);
}


void netlink::NetLinkCore::setLocalAddress(const std::string &ipv4)
{
	{
		std::lock_guard<std::mutex> lock(mAddressMutex);
		mLocalAddress = ipv4;
	}

	if (mInitialized.load())
		applyLocalAddress();
}


void netlink::NetLinkCore::applyLocalAddress()
{
	std::string address;
	{
		std::lock_guard<std::mutex> lock(mAddressMutex);
		address = mLocalAddress;
	}

	if (address.empty())
		return;

	mConnectionService.setLocalIP(address);
	mSignaling.setLocalIPv4(address);
	updateDiscoveryConfig();
}


void netlink::NetLinkCore::updateDiscoveryConfig()
{
	DiscoveryConfig discoveryConfig;

	{
		std::lock_guard<std::mutex> lock(mAddressMutex);
		discoveryConfig.localIPv4 = mLocalAddress;
	}

	discoveryConfig.displayName		 = mConfig.localDisplayName;
	discoveryConfig.discoveryPort	 = mConfig.discoveryPort;
	discoveryConfig.broadcastAddress = mConfig.broadcastAddress;
	discoveryConfig.signalingPort	 = mSignaling.getBoundPort();

	if (!mDiscovery.init(discoveryConfig))
		NETLINK_LOG_ERROR("Discovery could not be configured for {}", discoveryConfig.localIPv4);
}


// ---------------------------------------------------------------------------
// Discovery & connection
// ---------------------------------------------------------------------------

bool netlink::NetLinkCore::startDiscovery()
{
	try
	{
		mDiscovery.startDiscovery();
	}
	catch (const std::exception &e)
	{
		NETLINK_LOG_ERROR("Cannot start discovery (no local address selected?): {}", e.what());
		return false;
	}

	mSignaling.start();

	auto expected = ConnectionState::None;
	mState.compare_exchange_strong(expected, ConnectionState::Searching);
	return true;
}


void netlink::NetLinkCore::stopDiscovery()
{
	// Signaling keeps running: established peers still need it for invitations and disconnects
	mDiscovery.stopDiscovery();

	auto expected = ConnectionState::Searching;
	mState.compare_exchange_strong(expected, ConnectionState::None);
}


std::vector<netlink::Endpoint> netlink::NetLinkCore::getPotentialEndpoints()
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


void netlink::NetLinkCore::respondToConnection(bool accepted)
{
	auto remote = mConnectionService.getCurrentRemote();
	if (!remote.has_value())
		return;

	if (accepted)
		mConnectionService.acceptIncomingConnection(remote->displayName);
	else
		mConnectionService.declineIncomingConnection(remote->displayName, "User declined");
}


void netlink::NetLinkCore::disconnect()
{
	if (auto remote = mConnectionService.getCurrentRemote(); remote.has_value())
		mConnectionService.closeConnection(remote->displayName);
}


bool netlink::NetLinkCore::send(uint32_t type, const std::vector<uint8_t> &payload, DeliveryMode mode)
{
	if (mState.load() != ConnectionState::Connected)
		return false;

	mCommunication.write(type, payload, mode);
	return true;
}


void netlink::NetLinkCore::postEvent(Event event)
{
	mEvents.post([this, event = std::move(event)]() { event(mCallbacks); });
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

	postEvent([endpoint = toPublicEndpoint(result.remoteEndpoint)](const NetLinkCallbacks &callbacks)
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
		mCommunication.init(update.session);
		mState.store(ConnectionState::Connected);
		emitConnectionChanged(ConnectionState::Connected, update.message, update.endpoint);
		mCommunication.start(); // after the Connected event, so no message is delivered before it
		break;

	case ConnectionStatusUpdate::Type::InvitationReceived:
		mState.store(ConnectionState::PendingInbound);
		emitConnectionChanged(ConnectionState::PendingInbound, update.message, update.endpoint);
		break;

	case ConnectionStatusUpdate::Type::Failed:
	case ConnectionStatusUpdate::Type::Declined:
		mState.store(ConnectionState::Error);
		emitConnectionChanged(ConnectionState::Error, update.message, update.endpoint);
		break;

	case ConnectionStatusUpdate::Type::Closed:
		mCommunication.deinit();
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


void netlink::NetLinkCore::emitConnectionChanged(ConnectionState state, const std::string &message, const DiscoveryEndpoint &remote)
{
	postEvent([event = ConnectionEvent{state, message, toPublicEndpoint(remote)}](const NetLinkCallbacks &callbacks)
			  {
				  if (callbacks.onConnectionChanged)
					  callbacks.onConnectionChanged(event);
			  });
}


netlink::Endpoint netlink::NetLinkCore::toPublicEndpoint(const DiscoveryEndpoint &endpoint)
{
	return {endpoint.IPAddress, endpoint.port, endpoint.displayName};
}
