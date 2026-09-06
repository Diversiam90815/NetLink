/*
  ==============================================================================
	Module:         TCPServer
	Description:    Server implementation for TCP Connections
  ==============================================================================
*/


#include "TCPServer.h"
#include "NetLinkLog.h"


TCPServer::~TCPServer()
{
	stopAccept();
	mAcceptorSocket.close();
}


bool TCPServer::bindAndListen(const std::string &address, int port, int backlog)
{
	mAcceptorSocket = NetlinkSocket::createTCP();

	auto result		= mAcceptorSocket.bind(address, port, {});
	if (!result.succeeded())
	{
		NETLINK_LOG_ERROR("TCPServer bind failed: {}", result.getStatusString());
		return false;
	}

	mBoundPort = mAcceptorSocket.getBoundPort();
	NETLINK_LOG_INFO("TCPServer bound port = {}", mBoundPort);

	if (!mAcceptorSocket.listen(backlog))
	{
		NETLINK_LOG_ERROR("TCPServer listen failed");
		return false;
	}

	return true;
}


void TCPServer::startAccept()
{
	start();
}


void TCPServer::stopAccept()
{
	stop();
}


void TCPServer::setSessionHandler(SessionHandler handler)
{
	mSessionHandler = handler;
}


void TCPServer::respondToConnectionRequest(bool accepted)
{
	if (!mPendingSession)
		return;

	if (accepted)
	{
		NETLINK_LOG_INFO("Accepting connection from: {}", mPendingSession->socket().remote_endpoint().address().to_string().c_str());

		if (mSessionHandler)
			mSessionHandler(mPendingSession);
	}
	else
	{
		NETLINK_LOG_INFO("Rejecting connection from {}", mPendingSession->socket().remote_endpoint().address().to_string().c_str());
	}

	// Clear the pending session
	mPendingSession.reset();
}


void TCPServer::run()
{
	while (isRunning())
	{
		if (!mAcceptorSocket.accept(250))
			continue; // timeout

		auto acceptedSocket = mAcceptorSocket.takeAcceptedConnection();
		if (!acceptedSocket.isOpen())
			continue;

		auto newSession = std::make_shared<TCPSession>(std::move(acceptedSocket));
		mPendingSession = newSession;

		NETLINK_LOG_INFO("TCP accepted connection, bound port = {}", newSession->getBoundPort());

		if (mSessionHandler)
			mSessionHandler(mPendingSession);
	}
}


int	 TCPServer::getBoundPort() const
{
	return mBoundPort;
}
