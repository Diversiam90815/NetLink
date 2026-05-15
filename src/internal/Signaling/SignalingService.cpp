/*
  ==============================================================================
	Module:         SignalingService
	Description:    Verification of the remote before establishing a connection
  ==============================================================================
*/

#include "SignalingService.h"
#include "NetLinkLog.h"

using json = nlohmann::json;


netlink::SignalingService::SignalingService(asio::io_context &ioContext) : mSocket(ioContext)
{
	mIoContext = &ioContext;
}


netlink::SignalingService::~SignalingService()
{
	deinit();
}


bool netlink::SignalingService::init(const std::string &localIPv4)
{
	if (localIPv4.empty())
		return false;

	asio::error_code ec;
	mSocket.open(udp::v4(), ec);

	if (ec)
	{
		NETLINK_LOG_ERROR("Failed to open signaling socket: {}", ec.message());
		return false;
	}

	mSocket.set_option(asio::socket_base::reuse_address(true), ec);

	if (ec)
		NETLINK_LOG_ERROR("Failed to set reuse_address option: {}", ec.message());

	// Bind to port 0 (OS assign free port)
	udp::endpoint localEndpoint(asio::ip::make_address_v4(localIPv4), 0);
	mSocket.bind(localEndpoint, ec);

	if (ec)
	{
		NETLINK_LOG_ERROR("Failed to bind signaling socket: {}", ec.message());
		return false;
	}

	mBoundPort = static_cast<int>(mSocket.local_endpoint().port());
	NETLINK_LOG_INFO("SignalingService bound to port {}", mBoundPort);

	mInitialized.store(true);
	return true;
}


void netlink::SignalingService::deinit()
{
	asio::error_code ec;

	mSocket.cancel(ec);

	if (ec)
		NETLINK_LOG_ERROR("Error cancelling socket operations: {}", ec.message());

	mSocket.close(ec);

	if (ec)
		NETLINK_LOG_ERROR("Error closing signaling socket: {}", ec.message());

	stop();
	mBoundPort = 0;
	mInitialized.store(false);
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


void netlink::SignalingService::sendConnectRequest(const std::string &targetIP, int targetSignalingPort, const std::string &displayName)
{
	SignalPacket packet{};
	packet.signalType		   = SignalType::ConnectRequest;
	packet.senderIP			   = mSocket.local_endpoint().address().to_string();
	packet.senderPort		   = 0; // TCP port filled by caller or Impl
	packet.senderSignalingPort = mBoundPort;
	packet.displayName		   = displayName;

	sendPacket(targetIP, targetSignalingPort, packet);
}


void netlink::SignalingService::sendConnectAccept(const std::string &targetIP, int targetSignalingPort)
{
	SignalPacket packet{};
	packet.signalType		   = SignalType::ConnectAccept;
	packet.senderIP			   = mSocket.local_endpoint().address().to_string();
	packet.senderSignalingPort = mBoundPort;

	sendPacket(targetIP, targetSignalingPort, packet);
}


void netlink::SignalingService::sendConnectDecline(const std::string &targetIP, int targetSignalingPort)
{
	SignalPacket packet{};
	packet.signalType		   = SignalType::ConnectDecline;
	packet.senderIP			   = mSocket.local_endpoint().address().to_string();
	packet.senderSignalingPort = mBoundPort;

	sendPacket(targetIP, targetSignalingPort, packet);
}


void netlink::SignalingService::sendDisconnect(const std::string &targetIP, int targetSignalingPort)
{
	SignalPacket packet{};
	packet.signalType		   = SignalType::Disconnect;
	packet.senderIP			   = mSocket.local_endpoint().address().to_string();
	packet.senderSignalingPort = mBoundPort;

	sendPacket(targetIP, targetSignalingPort, packet);
}

void netlink::SignalingService::sendReadyFlag(const std::string &targetIP, int targetSignalingPort)
{
	SignalPacket packet{};
	packet.signalType		   = SignalType::ReadyFlag;
	packet.senderIP			   = mSocket.local_endpoint().address().to_string();
	packet.senderSignalingPort = mBoundPort;

	sendPacket(targetIP, targetSignalingPort, packet);
}


void netlink::SignalingService::sendValidationRequest(const std::string &displayName, RemoteRequest request)
{
	auto peer = resolvePeer(displayName);

	if (!peer.isValid())
		return;

	// @TODO: build packet and send
	SignalPacket packet;

	//	sendPacket(peer.IPv4, peer.signalingPort, );
}


void netlink::SignalingService::sendSecretResponse(const std::string &displayName, const std::string &secret)
{
	auto peer = resolvePeer(displayName);

	if (!peer.isValid())
		return;

	// @TODO: build packet and send
	SignalPacket packet;

	//	sendPacket(peer.IPv4, peer.signalingPort, );
}


void netlink::SignalingService::sendVersionResponse(const std::string &displayName, const std::string &version)
{
	auto peer = resolvePeer(displayName);

	if (!peer.isValid())
		return;

	// @TODO: build packet and send
	SignalPacket packet;

	//	sendPacket(peer.IPv4, peer.signalingPort, );
}


void				  netlink::SignalingService::sendValidationHandshake(const std::string &displayName) {}


netlink::PeerEndpoint netlink::SignalingService::resolvePeer(const std::string &displayName) const
{
	std::lock_guard<std::mutex> lock(mPeerRegistryMutex);

	auto						it = mPeerRegistry.find(displayName);
	auto						it = mPeerRegistry.find(computerName);
	if (it == mPeerRegistry.end())
	{
		NETLINK_LOG_WARNING("Cannot resolve peer {}: not registered", displayName);
		return {};
	}

	return it->second;
}


void netlink::SignalingService::run()
{
	receiveAsync();

	while (isRunning())
	{
		mIoContext->run_one();
		waitForEvent(50);
	}
}


void netlink::SignalingService::receiveAsync()
{
	if (!mInitialized.load())
		return;

	mSocket.async_receive_from(asio::buffer(mRecvBuffer), mSenderEndpoint, [this](const asio::error_code &error, size_t bytesReceived) { handleReceive(error, bytesReceived); });
}


void netlink::SignalingService::handleReceive(const asio::error_code &error, size_t bytesReceived)
{
	if (!error && bytesReceived > 0)
	{
		try
		{
			std::string	 data(mRecvBuffer.data(), bytesReceived);
			json		 j		= json::parse(data);
			SignalPacket packet = j.get<SignalPacket>();

			routePacket(packet);
		}
		catch (std::exception &e)
		{
			NETLINK_LOG_ERROR("Error parsing signal packet: {}", e.what());
		}
	}
	else if (error && error != asio::error::operation_aborted)
	{
		NETLINK_LOG_WARNING("Signaling receive error: {}", error.message());
	}

	// Continue listening if still running
	if (isRunning())
		receiveAsync();
}


void netlink::SignalingService::routePacket(const SignalPacket &packet)
{
	switch (packet.signalType)
	{
	case SignalType::ConnectRequest:
		if (mCallbacks.onConnectRequested)
			mCallbacks.onConnectRequested(packet);
		break;

	case SignalType::ConnectAccept:
		if (mCallbacks.onConnectAccepted)
			mCallbacks.onConnectAccepted(packet);
		break;

	case SignalType::ConnectDecline:
		if (mCallbacks.onConnectDeclined)
			mCallbacks.onConnectDeclined(packet);
		break;

	case SignalType::Disconnect:
		if (mCallbacks.onDisconnectReceived)
			mCallbacks.onDisconnectReceived(packet);
		break;

	case SignalType::ReadyFlag:
		if (mCallbacks.onReadyFlagReceived)
			mCallbacks.onReadyFlagReceived(packet);
		break;

	default: NETLINK_LOG_WARNING("Unknown signal type received: {}", static_cast<int>(packet.signalType)); break;
	}
}


void netlink::SignalingService::sendPacket(const std::string &targetIP, int targetPort, const SignalPacket &packet)
{
	if (!mInitialized.load())
	{
		NETLINK_LOG_ERROR("SignalingService not initialized -> cannot send.");
		return;
	}

	json			 j	 = packet;
	std::string		 msg = j.dump();

	udp::endpoint	 target(asio::ip::make_address_v4(targetIP), static_cast<unsigned short>(targetPort));
	asio::error_code ec;

	mSocket.send_to(asio::buffer(msg), target, 0, ec);

	if (ec)
		NETLINK_LOG_ERROR("Failed to send signal to {}:{} - {}", targetIP, targetPort, ec.message());
	else
		NETLINK_LOG_DEBUG("Signal sent to {}:{} (type={})", targetIP, targetPort, static_cast<int>(packet.signalType));
}
