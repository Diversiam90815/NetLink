/*
  ==============================================================================
	Module:         ConnectionService
	Description:    Orchestrates the full connection lifecycle with per-phase
					timeouts, ReadyFlag synchronization, and peer validation.
  ==============================================================================
*/

#include "ConnectionService.h"
#include "NetLinkLog.h"


netlink::ConnectionService::ConnectionService(asio::io_context &ioContext, SignalingService &signaling, ITransportFactory &transportFactory, PeerValidationService &validation)
	: mIoContext(ioContext), mSignaling(signaling), mTransportFactory(transportFactory), mValidation(validation)
{
	SignalingCallbacks cb;
	cb.onConnectRequested	= [this](const SignalPacket &pkt) { onSignalConnectRequested(pkt); };
	cb.onConnectAccepted	= [this](const SignalPacket &pkt) { onSignalConnectAccepted(pkt); };
	cb.onConnectDeclined	= [this](const SignalPacket &pkt) { onSignalConnectDeclined(pkt); };
	cb.onDisconnectReceived = [this](const SignalPacket &pkt) { onSignalDisconnect(pkt); };
	cb.onReadyFlagReceived	= [this](const SignalPacket &pkt) { onSignalReadyFlag(pkt); };
	mSignaling.setCallbacks(std::move(cb));
}


void netlink::ConnectionService::setCallbacks(ConnectionServiceCallbacks cb)
{
	std::lock_guard<std::mutex> lock(mMutex);
	mCallbacks = std::move(cb);
}


void netlink::ConnectionService::setConfig(const ConnectionConfig &config)
{
	std::lock_guard<std::mutex> lock(mMutex);
	mConfig = config;
}


void netlink::ConnectionService::setLocalIP(const std::string &ip)
{
	std::lock_guard<std::mutex> lock(mMutex);
	mLocalIP = ip;
}


void netlink::ConnectionService::requestConnection(const DiscoveryEndpoint &remote)
{
	std::lock_guard<std::mutex> lock(mMutex);

	if (mPhase != ConnectionPhase::Idle)
		return;

	auto result = mValidation.validatePeer(remote);
	if (!result.isReadyToConnect())
	{
		NETLINK_LOG_WARNING("Validation failed for {} — {}", remote.IPAddress, result.message);

		if (mCallbacks.onConnectionFailed)
			mCallbacks.onConnectionFailed("Peer validation failed: " + result.message);

		return;
	}

	mRemote		 = remote;
	mIsInitiator = true;
	mPhase		 = ConnectionPhase::RequestSent;

	mSignaling.sendConnectRequest(remote.IPAddress, remote.signalingPort, mLocalIP);

	mTimeoutService.startTimeout({ConnectionTimeouts::Invitation, remote.IPAddress}, mConfig.invitationTimeoutMs, [this](const TimeoutKey &key) { onTimeout(key); });

	NETLINK_LOG_INFO("Connection request sent to {}", remote.IPAddress);
}


void netlink::ConnectionService::respondToConnection(bool accepted)
{
	std::lock_guard<std::mutex> lock(mMutex);

	if (mPhase != ConnectionPhase::RequestReceived)
		return;

	if (accepted)
	{
		mSignaling.sendConnectAccept(mRemote.IPAddress, mRemote.signalingPort);
		mPhase = ConnectionPhase::Accepted;

		mTimeoutService.startTimeout({ConnectionTimeouts::Connection, mRemote.IPAddress}, mConfig.connectionTimeoutMs, [this](const TimeoutKey &key) { onTimeout(key); });

		beginTransportEstablishment();
	}
	else
	{
		mSignaling.sendConnectDecline(mRemote.IPAddress, mRemote.signalingPort);
		reset();
	}
}


void netlink::ConnectionService::disconnect()
{
	std::lock_guard<std::mutex> lock(mMutex);

	if (mRemote.isValid())
	{
		mSignaling.sendDisconnect(mRemote.IPAddress, mRemote.signalingPort);
		mTimeoutService.cancelByIdentifier(mRemote.IPAddress);
	}
	else
	{
		mTimeoutService.cancelAll();
	}

	reset();
}


netlink::ConnectionPhase netlink::ConnectionService::getPhase() const
{
	std::lock_guard<std::mutex> lock(mMutex);
	return mPhase;
}


// ---------------------------------------------------------------------------
// Signal handlers
// ---------------------------------------------------------------------------

void netlink::ConnectionService::onSignalConnectRequested(const SignalPacket &pkt)
{
	std::lock_guard<std::mutex> lock(mMutex);

	if (mPhase != ConnectionPhase::Idle)
	{
		mSignaling.sendConnectDecline(pkt.senderIP, pkt.senderSignalingPort);
		return;
	}

	DiscoveryEndpoint remote{pkt.senderIP, pkt.senderPort, pkt.senderSignalingPort, pkt.displayName};

	auto			  result = mValidation.validatePeer(remote);
	if (!result.isReadyToConnect())
	{
		NETLINK_LOG_WARNING("Auto-declining {} — validation failed: {}", pkt.senderIP, result.message);
		mSignaling.sendConnectDecline(pkt.senderIP, pkt.senderSignalingPort);
		return;
	}

	mRemote		 = remote;
	mIsInitiator = false;
	mPhase		 = ConnectionPhase::RequestReceived;

	NETLINK_LOG_INFO("Connection request received from {}", pkt.senderIP);

	if (mCallbacks.onConnectionRequested)
		mCallbacks.onConnectionRequested(mRemote);
}


void netlink::ConnectionService::onSignalConnectAccepted(const SignalPacket &pkt)
{
	std::lock_guard<std::mutex> lock(mMutex);

	if (mPhase != ConnectionPhase::RequestSent)
		return;

	mTimeoutService.cancelTimeout({ConnectionTimeouts::Invitation, pkt.senderIP});

	mPhase = ConnectionPhase::Accepted;

	mTimeoutService.startTimeout({ConnectionTimeouts::Connection, pkt.senderIP}, mConfig.connectionTimeoutMs, [this](const TimeoutKey &key) { onTimeout(key); });

	NETLINK_LOG_INFO("Connection accepted by {}", pkt.senderIP);

	beginTransportEstablishment();
}


void netlink::ConnectionService::onSignalConnectDeclined(const SignalPacket &pkt)
{
	std::lock_guard<std::mutex> lock(mMutex);

	NETLINK_LOG_INFO("Connection declined by {}", pkt.senderIP);

	mTimeoutService.cancelByIdentifier(pkt.senderIP);

	if (mCallbacks.onConnectionFailed)
		mCallbacks.onConnectionFailed("Remote declined the connection");

	reset();
}


void netlink::ConnectionService::onSignalDisconnect(const SignalPacket &pkt)
{
	std::lock_guard<std::mutex> lock(mMutex);

	NETLINK_LOG_INFO("Disconnect signal received from {}", pkt.senderIP);

	mTimeoutService.cancelByIdentifier(pkt.senderIP);

	if (mCallbacks.onDisconnected)
		mCallbacks.onDisconnected();

	reset();
}


void netlink::ConnectionService::onSignalReadyFlag(const SignalPacket &pkt)
{
	std::lock_guard<std::mutex> lock(mMutex);

	if (mPhase != ConnectionPhase::AwaitingReadyFlag)
		return;

	NETLINK_LOG_INFO("Ready flag received from {}", pkt.senderIP);

	mTimeoutService.cancelTimeout({ConnectionTimeouts::ReadyFlag, pkt.senderIP});
	mRemoteReadyFlag.store(true);

	checkBothReady();
}


// ---------------------------------------------------------------------------
// Flow stages
// ---------------------------------------------------------------------------

void netlink::ConnectionService::beginTransportEstablishment(PeerState &peer)
{
	// Caller holds mMutex
	mPhase			 = ConnectionPhase::EstablishingTransport;
	SessionRole role = determineRole(mLocalIP, mRemote.IPAddress);

	if (role == SessionRole::Acceptor)
	{
		NETLINK_LOG_INFO("Role: Acceptor -> listening for inbound transport");

		mServer = mTransportFactory.createServer(mIoContext);
		mServer->setSessionHandler(
			[this](ISession::pointer session)
			{
				std::lock_guard<std::mutex> lock(mMutex);
				mSession = session;
				sendLocalReadyFlag();
			});
		mServer->startAccept();
	}
	else
	{
		NETLINK_LOG_INFO("Role: Connector -> connecting to {} ({})", mRemote.displayName, mRemote.IPAddress);

		mClient = mTransportFactory.createClient(mIoContext);
		mClient->setConnectHandler(
			[this](ISession::pointer session)
			{
				std::lock_guard<std::mutex> lock(mMutex);
				mSession = session;
				sendLocalReadyFlag();
			});

		mClient->setConnectTimeoutHandler(
			[this]()
			{
				std::lock_guard<std::mutex> lock(mMutex);
				NETLINK_LOG_WARNING("Transport connect timed out");
				mTimeoutService.cancelByIdentifier(mRemote.IPAddress);
				if (mCallbacks.onConnectionFailed)
					mCallbacks.onConnectionFailed("Transport connection timed out");
				reset();
			});

		mClient->connect(mRemote.IPAddress, static_cast<unsigned short>(mRemote.port));
	}
}


void netlink::ConnectionService::sendLocalReadyFlag(PeerState &peer)
{
	// Caller holds mMutex
	mLocalReadyFlag.store(true);
	mPhase = ConnectionPhase::AwaitingReadyFlag;

	mSignaling.sendReadyFlag(mRemote.IPAddress, mRemote.signalingPort);

	mTimeoutService.startTimeout({ConnectionTimeouts::ReadyFlag, mRemote.IPAddress}, mConfig.readyFlagTimeoutMs, [this](const TimeoutKey &key) { onTimeout(key); });

	NETLINK_LOG_INFO("Local ready flag sent to {}", mRemote.IPAddress);

	checkBothReady();
}


void netlink::ConnectionService::checkBothReady(PeerState &peer)
{
	// Caller holds mMutex
	if (!mLocalReadyFlag.load() || !mRemoteReadyFlag.load())
		return;

	mTimeoutService.cancelTimeout({ConnectionTimeouts::Connection, mRemote.IPAddress});
	mTimeoutService.cancelTimeout({ConnectionTimeouts::ReadyFlag, mRemote.IPAddress});

	mPhase = ConnectionPhase::Connected;

	NETLINK_LOG_INFO("Both peers ready — connection established with {}", mRemote.IPAddress);

	if (mCallbacks.onConnected)
		mCallbacks.onConnected(mSession);
}


// ---------------------------------------------------------------------------
// Timeout + reset
// ---------------------------------------------------------------------------

void netlink::ConnectionService::onTimeout(const TimeoutKey &key)
{
	std::lock_guard<std::mutex> lock(mMutex);

	std::string					reason;

	if (key.category == ConnectionTimeouts::Invitation)
		reason = "Invitation timed out";
	else if (key.category == ConnectionTimeouts::Connection)
		reason = "Connection establishment timed out";
	else if (key.category == ConnectionTimeouts::ReadyFlag)
		reason = "Ready flag timed out";
	else
		reason = "Connection timed out";

	NETLINK_LOG_WARNING("Timeout [{}]: {}", key.toString(), reason);

	mTimeoutService.cancelByIdentifier(key.identifier);

	if (mCallbacks.onConnectionFailed)
		mCallbacks.onConnectionFailed(reason);

	reset();
}


void netlink::ConnectionService::reset()
{
	// Caller holds mMutex (or called from destructor path)
	mPhase		 = ConnectionPhase::Idle;
	mRemote		 = {};
	mIsInitiator = false;
	mLocalReadyFlag.store(false);
	mRemoteReadyFlag.store(false);
	mSession.reset();
	mServer.reset();
	mClient.reset();
	mTimeoutService.cancelAll();
}
