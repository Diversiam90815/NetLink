/*
==============================================================================
	Module:         WinStreamSocket
	Description:    Low-level WinSock2 TCP socket
  ==============================================================================
*/

#include "WinStreamSocket.h"
#include "NetLinkLog.h"


WinStreamSocket::~WinStreamSocket()
{
	close();
}


NetLink::SocketBindResult WinStreamSocket::bind(const std::string &address, const int port, const NetLink::BindOptions &options)
{
	NetLink::SocketBindResult result;
	result.address		 = address;
	result.requestedPort = port;

	close();

	mSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (mSocket == INVALID_SOCKET)
	{
		result.nativeError = WSAGetLastError();
		return result;
	}

	applyOptions(mSocket, options);

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


bool WinStreamSocket::listen(int backlog)
{
	if (mSocket == INVALID_SOCKET || mClosed.load())
		return false;

	if (::listen(mSocket, backlog) == SOCKET_ERROR)
	{
		NETLINK_LOG_ERROR("listen failed: WSA error {}", WSAGetLastError());
		return false;
	}

	return true;
}


bool WinStreamSocket::accept(int timeoutMS)
{
	if (mSocket == INVALID_SOCKET || mClosed.load())
		return false;

	if (!waitOn(mSocket, true, timeoutMS))
		return false; // timeout / not ready

	sockaddr_in peerAddr{};
	int			peerLen	 = sizeof(peerAddr);

	SOCKET		accepted = ::accept(mSocket, reinterpret_cast<sockaddr *>(&peerAddr), &peerLen);

	if (accepted == INVALID_SOCKET)
	{
		NETLINK_LOG_ERROR("accept failed: WSA error {}", WSAGetLastError());
		return false;
	}

	if (mConnected != INVALID_SOCKET)
		closesocket(mConnected);

	mConnected = accepted;

	char ipStr[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &peerAddr.sin_addr, ipStr, sizeof(ipStr));
	mRemoteAddress = ipStr;
	mRemotePort	   = ntohs(peerAddr.sin_port);

	return true;
}


bool WinStreamSocket::connect(const std::string &address, int port, int timeoutMS)
{
	if (mSocket == INVALID_SOCKET || mClosed.load())
		return false;

	// switch to non-blocking to enforce timeout on connect()
	u_long mode = 1;
	ioctlsocket(mSocket, FIONBIO, &mode);

	sockaddr_in destAddr{};
	destAddr.sin_family = AF_INET;
	destAddr.sin_port	= htons(static_cast<u_short>(port));
	inet_pton(AF_INET, address.c_str(), &destAddr.sin_addr);

	int result = ::connect(mSocket, reinterpret_cast<sockaddr *>(&destAddr), sizeof(destAddr));

	if (result == SOCKET_ERROR)
	{
		int error = WSAGetLastError();

		if (error != WSAEWOULDBLOCK)
		{
			NETLINK_LOG_ERROR("connect failed: WSA error {} (host:{}, port:{})", error, address, port);
			return false;
		}

		if (!waitOn(mSocket, false, timeoutMS))
			return false; // timeout

		int soError	   = 0;
		int soErrorLen = sizeof(soError);
		getsockopt(mSocket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&soError), &soErrorLen);

		if (soError != 0)
		{
			NETLINK_LOG_ERROR("connect failed post-select: WSA error {}", soError);
			return false;
		}
	}

	// restore blocking mode for subsequent send/recv (we manage timeouts via select ourselves)
	mode = 0;
	ioctlsocket(mSocket, FIONBIO, &mode);

	mConnected	   = mSocket; // client mode: same descriptor is both "connecting" and "data" socket
	mRemoteAddress = address;
	mRemotePort	   = port;

	return true;
}


int WinStreamSocket::sendTo(const std::string &address, const int port, const void *data, int size)
{
	if (mConnected == INVALID_SOCKET || mClosed.load())
		return -1;

	int result = ::send(mConnected, static_cast<const char *>(data), size, 0);

	if (result < 0)
	{
		int error = WSAGetLastError();

		if (error != WSAEWOULDBLOCK)
			NETLINK_LOG_ERROR("send failed: WSA error {} (size:{})", error, size);
	}

	return result;
}


int WinStreamSocket::recvFrom(void *buffer, int maxSize, std::string &remoteAddress, int &remotePort, int timeoutMS)
{
	if (mConnected == INVALID_SOCKET || mClosed.load())
		return -1;

	if (!waitOn(mConnected, true, timeoutMS))
		return 0; // timeout

	int nBytes = ::recv(mConnected, static_cast<char *>(buffer), maxSize, 0);

	if (nBytes == 0)
		return -1; // peer performed an orderly shutdown

	if (nBytes > 0)
	{
		remoteAddress = mRemoteAddress;
		remotePort	  = mRemotePort;
	}

	return nBytes;
}


bool WinStreamSocket::waitUntilReady(bool forReading, int timeoutMS)
{
	SOCKET sock = (mConnected != INVALID_SOCKET) ? mConnected : mSocket;
	return waitOn(sock, forReading, timeoutMS);
}


void WinStreamSocket::close()
{
	mClosed.store(true);

	if (mConnected != INVALID_SOCKET && mConnected != mSocket)
	{
		::shutdown(mConnected, SD_BOTH);
		closesocket(mConnected);
	}

	mConnected = INVALID_SOCKET;

	if (mSocket != INVALID_SOCKET)
	{
		::shutdown(mSocket, SD_BOTH);
		closesocket(mSocket);
		mSocket = INVALID_SOCKET;
	}

	mBoundPort = 0;
	mRemoteAddress.clear();
	mRemotePort = 0;
}


void WinStreamSocket::applyOptions(SOCKET sock, const NetLink::BindOptions &options)
{
	auto setOpt = [sock](int level, int name, int value, const char *label)
	{
		if (setsockopt(sock, level, name, reinterpret_cast<const char *>(&value), sizeof(value)) == SOCKET_ERROR)
			NETLINK_LOG_WARNING("setsockopt({}) failed: WSA: {}", label, WSAGetLastError());
	};

	if (options.reuseAddress)
		setOpt(SOL_SOCKET, SO_REUSEADDR, 1, "SO_REUSEADDR");

	if (options.receiveBufferSize > 0)
		setOpt(SOL_SOCKET, SO_RCVBUF, options.receiveBufferSize, "SO_RCVBUF");

	if (options.sendBufferSize > 0)
		setOpt(SOL_SOCKET, SO_SNDBUF, options.sendBufferSize, "SO_SNDBUF");
}


bool WinStreamSocket::waitOn(SOCKET sock, bool forReading, int timeoutMS) const
{
	if (sock == INVALID_SOCKET)
		return false;

	fd_set fdSet;
	FD_ZERO(&fdSet);
	FD_SET(sock, &fdSet);

	timeval tv;
	tv.tv_sec  = timeoutMS / 1000;
	tv.tv_usec = (timeoutMS % 1000) * 1000;

	int result = select(0, forReading ? &fdSet : nullptr, forReading ? nullptr : &fdSet, nullptr, &tv);
	return result > 0;
}
