/*
==============================================================================
	Module:         WinDatagramSocket
	Description:    Low-level WinSock2 UDP socket
  ==============================================================================
*/


#include "WinDatagramSocket.h"

WinsockScope::WinsockScope()
{
}

WinsockScope::~WinsockScope()
{
}


WinDatagramSocket::~WinDatagramSocket()
{
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