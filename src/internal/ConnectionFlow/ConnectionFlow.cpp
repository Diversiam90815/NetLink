/*
  ==============================================================================
	Module:         ConnectionFlow
	Description:    Orchestrating the connection
  ==============================================================================
*/

#include "ConnectionFlow.h"
#include "NetLinkLog.h"


netlink::ConnectionFlow::ConnectionFlow(asio::io_context &io_context, SignalingService &signaling, ITransportFactory &transportFactory) : mIoContext(io_context), mSignaling(signaling), mTransportFactory(transportFactory)
{

	// wire signaling with connectionflow
	SignalingCallbacks cb;
	cb.onConnectRequested	= [this](const SignalPacket &pkt) { onSignalConnectRequested(pkt); };
	cb.onConnectAccepted	= [this](const SignalPacket &pkt) { onSignalConnectAccepted(pkt); };
	cb.onConnectDeclined	= [this](const SignalPacket &pkt) { onSignalConnectDeclined(pkt); };
	cb.onDisconnectReceived = [this](const SignalPacket &pkt) { onSignalDisconnect(pkt); };
	mSignaling.setCallbacks(std::move(cb));
}


void netlink::ConnectionFlow::requestConnection(const DiscoveryEndpoint &remote)
{
	if (mPhase != ConnectionPhase::Idle)
		return;

	mRemote = remote;
	mPhase	= ConnectionPhase::RequestSent;
	mSignaling.sendConnectRequest(remote.IPAddress, remote.signalingPort, mLocalIP); // @TODO
	NETLINK_LOG_INFO("Connection request sent to {}", remote.IPAddress);
}


void netlink::ConnectionFlow::respondToConnection(bool accepted)
{
	if (mPhase != ConnectionPhase::RequestReceived)
		return;

	if (accepted)
	{
		mSignaling.sendConnectAccept(mRemote.IPAddress, mRemote.signalingPort);
		mPhase = ConnectionPhase::Accepted;
		beginTransportEstablishment();
	}
	else
	{
		mSignaling.sendConnectDecline(mRemote.IPAddress, mRemote.signalingPort);
		reset();
	}
}


void netlink::ConnectionFlow::disconnect()
{
	if (mRemote.isValid())
		mSignaling.sendDisconnect(mRemote.IPAddress, mRemote.signalingPort);

	reset();
}


void netlink::ConnectionFlow::onSignalConnectRequested(const SignalPacket &pkt)
{
	if (mPhase != ConnectionPhase::Idle)
	{
		// Already in a connection flow
		mSignaling.sendConnectDecline(pkt.senderIP, pkt.senderSignalingPort);
		return;
	}

	mRemote = {pkt.senderIP, pkt.senderPort, pkt.senderSignalingPort, pkt.displayName};
	mPhase	= ConnectionPhase::RequestReceived;

	if (mCallbacks.onConnectionRequested)
		mCallbacks.onConnectionRequested(mRemote);
}


void netlink::ConnectionFlow::onSignalConnectAccepted(const SignalPacket &pkt)
{
	if (mPhase != ConnectionPhase::RequestSent)
		return;

	mPhase = ConnectionPhase::Accepted;
	beginTransportEstablishment();
}


void netlink::ConnectionFlow::onSignalConnectDeclined(const SignalPacket &pkt)
{
	if (mCallbacks.onConnectionFailed)
		mCallbacks.onConnectionFailed("Remote declined the connection");

	reset();
}


void netlink::ConnectionFlow::onSignalDisconnect(const SignalPacket &pkt)
{
	if (mCallbacks.onDisconnected)
		mCallbacks.onDisconnected();

	reset();
}

void netlink::ConnectionFlow::beginTransportEstablishment()
{
	mPhase			 = ConnectionPhase::EstablishingTransport;
	SessionRole role = determineRole(mLocalIP, mRemote.IPAddress);

	if (role == SessionRole::Acceptor)
	{
		NETLINK_LOG_INFO("Role: Acceptor -> Listening for inbound connection");

		mServer = mTransportFactory.createServer(mIoContext);
		mServer->setSessionHandler(
			[this](ISession::pointer session)
			{
				mPhase = ConnectionPhase::Connected;
				if (mCallbacks.onConnected)
					mCallbacks.onConnected(session);
			});
		mServer->startAccept();
	}
	else
	{
		NETLINK_LOG_INFO("Role: Connector -> connecting to {}-({})", mRemote.displayName, mRemote.IPAddress);

		mClient = mTransportFactory.createClient(mIoContext);
		mClient->setConnectHandler(
			[this](ISession::pointer session)
			{
				mPhase = ConnectionPhase::Connected;
				if (mCallbacks.onConnected)
					mCallbacks.onConnected(session);
			});

		mClient->setConnectTimeoutHandler(
			[this]()
			{
				if (mCallbacks.onConnectionFailed)
					mCallbacks.onConnectionFailed("Connection timed out");

				reset();
			});
		mClient->connect(mRemote.IPAddress, static_cast<unsigned short>(mRemote.port));
	}
}


void netlink::ConnectionFlow::reset()
{
	mPhase	= ConnectionPhase::Idle;
	mRemote = {};
	mServer.reset();
	mClient.reset();
}
