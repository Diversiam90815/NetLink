/*
==============================================================================
	Module:         WinDatagramSocket
	Description:    Low-level WinSock2 UDP socket
  ==============================================================================
*/


#include "WinDatagramSocket.h"
#include "NetLinkLog.h"


namespace
{
std::mutex gWinsSocketMutex;
int		   gWinsSocketRefCount = 0;
} // namespace


WinsockScope::WinsockScope()
{
	std::lock_guard<std::mutex> lock(gWinsSocketMutex);

	if (gWinsSocketRefCount++ == 0)
	{
		WSADATA	  wsaData{};
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
	result.address		 = address;
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
	addr.sin_port	= htons(static_cast<u_short>(port));

	if (address.empty())
		addr.sin_addr.s_addr = INADDR_ANY;
	else if (inet_pton(AF_INET, address.c_str(), &addr.sin_addr.s_addr) != 1)
	{
		result.nativeError = WSAEINVAL;
		closesocket(mSocket);
		mSocket = INVALID_SOCKET;
		return result;
	}

	if (::bind(mSocket, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR)
	{
		result.nativeError = WSAGetLastError();
		closesocket(mSocket);
		mSocket = INVALID_SOCKET;
		return result;
	}

	// retrieve actual port
	sockaddr_in boundAddr{};
	int			addrLen = sizeof(boundAddr);
	getsockname(mSocket, reinterpret_cast<sockaddr *>(&boundAddr), &addrLen);
	mBoundPort		 = ntohs(boundAddr.sin_port);

	mClosed			 = false;
	result.success	 = true;
	result.boundPort = mBoundPort;
	result.address	 = address;
	return result;
}


int WinDatagramSocket::sendTo(const std::string &address, const int port, const void *data, int size)
{
	if (mSocket == INVALID_SOCKET || mClosed.load())
		return -1;

	sockaddr_in destAddr{};
	destAddr.sin_family = AF_INET;
	destAddr.sin_port	= htons(static_cast<u_short>(port));
	inet_pton(AF_INET, address.c_str(), &destAddr.sin_addr);

	int result = sendto(mSocket, static_cast<const char *>(data), size, 0, reinterpret_cast<sockaddr *>(&destAddr), sizeof(destAddr));

	if (result < 0)
	{
		int error = WSAGetLastError();

		if (error != WSAEWOULDBLOCK)
			NETLINK_LOG_ERROR("sendto failed: WSA error {} (host:{}, port:{}, size:{}", error, address, port, size);
	}

	return result;
}


int WinDatagramSocket::recvFrom(void *buffer, int maxSize, std::string &remoteAddress, int &remotePort, int timeoutMS)
{
	if (mSocket == INVALID_SOCKET || mClosed.load())
		return -1;

	if (!waitUntilReady(true, timeoutMS))
		return 0; // timeout

	sockaddr_in fromAddr{};
	int			fromLen = sizeof(fromAddr);

	int			nBytes	= recvfrom(mSocket, static_cast<char *>(buffer), maxSize, 0, reinterpret_cast<sockaddr *>(&fromAddr), &fromLen);

	if (nBytes > 0)
	{
		char ipStr[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &fromAddr.sin_addr, ipStr, sizeof(ipStr));
		remoteAddress = ipStr;
		remotePort	  = ntohs(fromAddr.sin_port);
	}

	return nBytes;
}


bool WinDatagramSocket::waitUntilReady(bool forReading, int timeoutMS)
{
	if (mSocket == INVALID_SOCKET)
		return false;

	fd_set fdSet;
	FD_ZERO(&fdSet);
	FD_SET(mSocket, &fdSet);

	timeval tv;
	tv.tv_sec  = timeoutMS / 1000;
	tv.tv_usec = (timeoutMS % 1000) * 1000;

	int result = select(0, forReading ? &fdSet : nullptr, forReading ? nullptr : &fdSet, nullptr, &tv);
	return result > 0;
}


void WinDatagramSocket::close()
{
	mClosed.store(true);

	if (mSocket != INVALID_SOCKET)
	{
		::shutdown(mSocket, SD_BOTH);
		closesocket(mSocket);
		mSocket = INVALID_SOCKET;
	}

	mBoundPort = 0;
}


void WinDatagramSocket::applyOptions(const NetLink::BindOptions &options)
{
	auto setOpt = [this](int level, int name, int value, const char *label)
	{
		if (setsockopt(mSocket, level, name, reinterpret_cast<const char *>(&value), sizeof(value)) == SOCKET_ERROR)
			NETLINK_LOG_WARNING("setsockopt({}) failed: WSA: {}", label, WSAGetLastError());
	};

	if (options.enableBroadcast)
		setOpt(SOL_SOCKET, SO_BROADCAST, 1, "SO_BROADCAST");

	if (options.receiveBufferSize > 0)
		setOpt(SOL_SOCKET, SO_RCVBUF, options.receiveBufferSize, "SO_RCVBUF");

	if (options.sendBufferSize > 0)
		setsockopt(SOL_SOCKET, SO_SNDBUF, options.sendBufferSize, "SO_SNDBUF");

	if (options.reuseAddress)
		setOpt(SOL_SOCKET, SO_REUSEADDR, 1, "SO_REUSEADDR");
}
