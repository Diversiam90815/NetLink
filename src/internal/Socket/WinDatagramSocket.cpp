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
		WSADATA   wsaData{};
		const int rc = WSAStartup(MAKEWORD(2, 2), &wsaData);

		if (rc != 0)
			gWinsSocketRefCount = 0;
	}
}

WinsockScope::~WinsockScope()
{
	std::lock_guard<std::mutex> lock(gWinsSocketMutex);

	if (gWinsSocketRefCount > 0 && --gWinsSocketRefCount == 0)
		WSACleanup();
}


WinDatagramSocket::~WinDatagramSocket()
{
	close();
}


NetLink::SocketBindResult WinDatagramSocket::bind(const std::string &address, const int port, const NetLink::BindOptions &options)
{
	NetLink::SocketBindResult result;
	result.address       = address;
	result.requestedPort = port;

	close();

	mSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	if (mSocket == INVALID_SOCKET)
	{
		result.nativeError = WSAGetLastError();
		return result;
	}

	applyOptions(options);

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port   = htons(static_cast<u_short>(port));

	if (address.empty())
		addr.sin_addr.s_addr = INADDR_ANY;
	else if (inet_pton(AF_INET, address.c_str(), &addr.sin_addr.s_addr) != 1)
	{
		result.nativeError = WSAEINVAL;
		closesocket(mSocket);
		mSOCKET = INVALID_SOCKET;
		return result;
	}

	if (::bind(mSocket, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR)
	{
		result.nativeError = WSAGetLastError();
		closesocket(mSocket);
		mSOCKET = INVALID_SOCKET;
		return result;
	}

	// retrieve actual port
	sockaddr boundAddr{};
	int      addrLen = sizeof(boundAddr);
	getsockname(mSocket, &boundAddr, &addrLen);
	mBoundPort = ntohs(boundAddr.sin_port);

	mClosed          = false;
	result.success   = true;
	result.boundPort = mBoundPort;
	result.address   = address;
	return result;
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