/*
  ==============================================================================
	Module:         SignalingService
	Description:    Verification of the remote before establishing a connection
  ==============================================================================
*/

#include "SignalingService.h"
#include "NetLinkLog.h"

using json = nlohmann::json;


netlink::SignalingService::~SignalingService()
{
	deinit();
}


bool netlink::SignalingService::init(const std::string &localComputerName)
{
	if (localComputerName.empty())
		return false;

	mLocalComputerName = localComputerName;
	mSocket			   = NetlinkSocket::createUDP();

	// Socket opened, binding deferred until setLocalIPv4() is called
	mInitialized.store(true);
	return true;
}


void netlink::SignalingService::deinit()
{
	stop();
	mSocket.close();
	mBoundPort = 0;
	mInitialized.store(false);
}


void netlink::SignalingService::setLocalIPv4(const std::string &localIPv4)
{
	if (localIPv4.empty())
		return;

	// Rebind if already bound to a previous address
	if (mBoundPort != 0)
		mSocket = NetlinkSocket::createUDP();

	mLocalIPv4 = localIPv4;

	NetLink::BindOptions options;
	options.reuseAddress = true;

	auto result			 = mSocket.bind(localIPv4, 0, options);

	if (!result.succeeded())
	{
		NETLINK_LOG_ERROR("Failed to bind signaling socket to {}: {}", localIPv4, result.getStatusString());
		return;
	}

	mBoundPort = mSocket.getBoundPort();
	NETLINK_LOG_INFO("SignalingService bound to {}:{}", localIPv4, mBoundPort);

	if (mOnSocketBound)
		mOnSocketBound(mBoundPort);
}


void netlink::SignalingService::registerPeer(const std::string &displayName, const std::string &ipv4, const int signalingPort)
{
	std::lock_guard<std::mutex> lock(mPeerRegistryMutex);
	mPeerRegistry[displayName] = {ipv4, signalingPort};
	NETLINK_LOG_DEBUG("Registered peer {} -> {}:{}", displayName, ipv4, signalingPort);
}


void netlink::SignalingService::unregisterPeer(const std::string &displayName)
{
	std::lock_guard<std::mutex> lock(mPeerRegistryMutex);
	mPeerRegistry.erase(displayName);
	NETLINK_LOG_DEBUG("Unregistered peer {}", displayName);
}


void netlink::SignalingService::sendConnectRequest(const std::string &computerName)
{
	auto peer = resolvePeer(computerName);
	if (!peer.isValid())
		return;

	auto packet = makeEnvelope(SignalType::ConnectRequest);

	sendPacket(peer, packet);
}


void netlink::SignalingService::sendConnectAnswer(const std::string &computerName, bool requestAccepted)
{
	auto peer = resolvePeer(computerName);
	if (!peer.isValid())
		return;

	auto packet	   = makeEnvelope(SignalType::ConnectAnswer);
	packet.payload = PayloadConnectAnswer{requestAccepted};

	sendPacket(peer, packet);
}


void netlink::SignalingService::sendDisconnect(const std::string &computerName)
{
	auto peer = resolvePeer(computerName);
	if (!peer.isValid())
		return;

	auto packet = makeEnvelope(SignalType::Disconnect);

	sendPacket(peer, packet);
}


void netlink::SignalingService::sendReadyFlag(const std::string &computerName)
{
	auto peer = resolvePeer(computerName);
	if (!peer.isValid())
		return;

	auto packet = makeEnvelope(SignalType::ReadyFlag);

	sendPacket(peer, packet);
}


void netlink::SignalingService::sendDataPort(const std::string &computerName, int dataPort)
{
	auto peer = resolvePeer(computerName);
	if (!peer.isValid())
		return;

	auto packet	   = makeEnvelope(SignalType::DataPort);
	packet.payload = PayloadDataPort{dataPort};

	sendPacket(peer, packet);
}


void netlink::SignalingService::sendValidationRequest(const std::string &computerName, RemoteRequest request)
{
	auto peer = resolvePeer(computerName);
	if (!peer.isValid())
		return;

	auto packet	   = makeEnvelope(SignalType::ValidationRequest);
	packet.payload = PayloadValidationRequest{static_cast<uint8_t>(request)};

	sendPacket(peer, packet);
}


void netlink::SignalingService::sendSecretResponse(const std::string &computerName, const std::string &secret)
{
	auto peer = resolvePeer(computerName);

	if (!peer.isValid())
		return;

	auto packet	   = makeEnvelope(SignalType::SecretResponse);
	packet.payload = PayloadSecretResponse{(secret)};

	sendPacket(peer, packet);
}


void netlink::SignalingService::sendVersionResponse(const std::string &computerName, const std::string &version)
{
	auto peer = resolvePeer(computerName);
	if (!peer.isValid())
		return;

	auto packet	   = makeEnvelope(SignalType::VersionResponse);
	packet.payload = PayloadVersionResponse{(version)};

	sendPacket(peer, packet);
}


void netlink::SignalingService::sendValidationHandshake(const std::string &computerName)
{
	auto peer = resolvePeer(computerName);
	if (!peer.isValid())
		return;

	auto packet = makeEnvelope(SignalType::ValidationHandshake);

	sendPacket(peer, packet);
}


netlink::PeerEndpoint netlink::SignalingService::resolvePeer(const std::string &computerName) const
{
	std::lock_guard<std::mutex> lock(mPeerRegistryMutex);

	auto						it = mPeerRegistry.find(computerName);
	if (it == mPeerRegistry.end())
	{
		NETLINK_LOG_WARNING("Cannot resolve peer {}: not registered", computerName);
		return {};
	}

	return it->second;
}


void netlink::SignalingService::run()
{
	while (isRunning())
	{
		receivePackage();
		waitForEvent(50);
	}
}


void netlink::SignalingService::routePacket(const SignalPacket &packet)
{
	const std::string &sender = packet.senderName;

	switch (packet.signalType)
	{
	case SignalType::ConnectRequest:
	{
		if (mCallbacks.onConnectRequested)
			mCallbacks.onConnectRequested(sender);
		break;
	}

	case SignalType::ConnectAnswer:
	{
		const auto &pl = std::get<PayloadConnectAnswer>(packet.payload);
		if (mCallbacks.onConnectRequestAnswered)
			mCallbacks.onConnectRequestAnswered(sender, pl.accepted);
		break;
	}

	case SignalType::Disconnect:
		if (mCallbacks.onDisconnectReceived)
			mCallbacks.onDisconnectReceived(sender);
		break;

	case SignalType::ReadyFlag:
		if (mCallbacks.onReadyFlagReceived)
			mCallbacks.onReadyFlagReceived(sender);
		break;

	case SignalType::DataPort:
	{
		const auto &pl = std::get<PayloadDataPort>(packet.payload);
		if (mCallbacks.onDataPortReceived)
			mCallbacks.onDataPortReceived(sender, pl.dataPort);
		break;
	}

	case SignalType::ValidationRequest:
	{
		const auto &pl = std::get<PayloadValidationRequest>(packet.payload);
		if (mCallbacks.onValidationRequestReceived)
			mCallbacks.onValidationRequestReceived(sender, pl.request);
		break;
	}

	case SignalType::SecretResponse:
	{
		const auto &pl = std::get<PayloadSecretResponse>(packet.payload);
		if (mCallbacks.onSecretResponseReceived)
			mCallbacks.onSecretResponseReceived(sender, pl.secret);
		break;
	}

	case SignalType::VersionResponse:
	{
		const auto &pl = std::get<PayloadVersionResponse>(packet.payload);
		if (mCallbacks.onVersionResponseReceived)
			mCallbacks.onVersionResponseReceived(sender, pl.version);
		break;
	}

	case SignalType::ValidationHandshake:
		if (mCallbacks.onValidationHandshakeReceived)
			mCallbacks.onValidationHandshakeReceived(sender);
		break;

	default: NETLINK_LOG_WARNING("Unknown signal type received: {}", static_cast<int>(packet.signalType)); break;
	}
}


void netlink::SignalingService::sendPacket(const PeerEndpoint &endpoint, const SignalPacket &packet)
{
	if (!mInitialized.load())
	{
		NETLINK_LOG_ERROR("SignalingService not initialized -> cannot send.");
		return;
	}

	json		j	 = packet;
	std::string msg	 = j.dump();

	int			sent = mSocket.sendTo(endpoint.IPv4, endpoint.signalingPort, msg.data(), static_cast<int>(msg.size()));

	if (sent < 0)
		NETLINK_LOG_ERROR("Failed to send signal to {}:{}", endpoint.IPv4, endpoint.signalingPort);
	else
		NETLINK_LOG_DEBUG("Signal sent to {}:{} (type={})", endpoint.IPv4, endpoint.signalingPort, static_cast<int>(packet.signalType));
}


void netlink::SignalingService::receivePackage()
{
	if (!mInitialized.load())
		return;

	ReceivedPacket packet;
	if (!mSocket.receive(packet, 50))
		return; // timeout

	try
	{
		json		 j		= json::parse(packet.data.begin(), packet.data.end());
		SignalPacket signal = j.get<SignalPacket>();

		routePacket(signal);
	}
	catch (std::exception &e)
	{
		NETLINK_LOG_ERROR("Error parsing signal packet: {}", e.what());
	}
}


netlink::SignalPacket netlink::SignalingService::makeEnvelope(SignalType type) const
{
	SignalPacket packet{};

	packet.senderName = mLocalComputerName;
	packet.senderIP	  = mLocalIPv4;
	packet.senderPort = mBoundPort;
	packet.signalType = type;

	return packet;
}
