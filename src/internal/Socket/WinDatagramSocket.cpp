/*
==============================================================================
	Module:         WinDatagramSocket
	Description:    Low-level WinSock2 UDP socket
  ==============================================================================
*/


#include "WinDatagramSocket.h"

namespace
{
std::mutex gWinsSocketMutex;
int        gWinsSocketRefCount = 0;
}


WinsockScope::WinsockScope()
{
	std::lock_guard<std::mutex> lock(gWinsSocketMutex);

	if (gWinsSocketRefCount++ == 0)
	{
		WSADATA wsaData{};
		const int rc = WSAStartup(MAKEWORD(2, 2), &wsaData);

		if (rc != 0)
			gWinsSocketRefCount = 0;
	}
}

WinsockScope::~WinsockScope()
{
	std::lock_guard<std::mutex> lock(gWinsSocketMutex);

	if (gWinsSocketRefCount> 0 && --gWinsSocketRefCount == 0)
		WSACleanup();
}


WinDatagramSocket::~WinDatagramSocket()
{
	close();
}


NetLink::SocketBindResult WinDatagramSocket::bind(const std::string &address, const int port, const NetLink::BindOptions &options)
{
	return NetLink::SocketBindResult();
}


int WinDatagramSocket::sendTo(const std::string &address, const int port, const void *data, int size)
{
	return 0;
}


int WinDatagramSocket::recvFrom(void *buffer, int maxSize, std::string &remoteAddress, int &remotePort, int timeoutMS)
{
	return 0;
}


bool WinDatagramSocket::waitUntilReady(bool forReading, int timeoutMS)
{
	return false;
}


void WinDatagramSocket::close()
{
}