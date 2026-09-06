/*
  ==============================================================================
	Module:         TCPClient
	Description:    Client implementation used for the multiplayer mode
  ==============================================================================
*/

#include "TCPClient.h"
#include "NetLinkLog.h"
#include "../Socket/NetlinkSocket.h"



TCPClient::~TCPClient()
{
	if (mConnectThread.joinable())
		mConnectThread.join();
}


void TCPClient::connect(const std::string &host, unsigned short port)
{
	if (mConnectThread.joinable())
		mConnectThread.join();

	mConnectThread = std::thread(
		[this, host, port]()
		{
			auto sock = NetlinkSocket::createTCP();
			sock.bind("", 0, {}); // ephemeral local port

			bool ok = sock.connect(host, port, mTimeoutInSeconds * 1000);

			if (ok)
			{
				auto session = std::make_shared<TCPSession>(std::move(sock));
				if (mConnectHandler)
					mConnectHandler(session);
			}
			else if (mConnectTimeoutHandler)
			{
				mConnectTimeoutHandler();
			}
		});
}


void TCPClient::setConnectHandler(ConnectHandler handler)
{
	mConnectHandler = handler;
}


void TCPClient::setConnectTimeoutHandler(ConnectTimeoutHandler handler)
{
	mConnectTimeoutHandler = handler;
}
