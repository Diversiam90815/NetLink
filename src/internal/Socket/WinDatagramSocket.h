/*
==============================================================================
	Module:         WinDatagramSocket
	Description:    Low-level WinSock2 UDP socket
  ==============================================================================
*/

#pragma once

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <atomic>
#include <mutex>
#include <string>

#include "ISocket.h"

#pragma comment(lib, "Ws2_32.lib")

struct WinsockScope
{
	WinsockScope();
	~WinsockScope();
	WinsockScope(const WinsockScope &) = delete;
	WinsockScope &operator=(const WinsockScope &) = delete;
};


class WinDatagramSocket : public ISocket
{
public:
	WinDatagramSocket() = default;
	~WinDatagramSocket() override;
	WinDatagramSocket(const WinDatagramSocket &) = delete;
	WinDatagramSocket &operator=(const WinDatagramSocket &) = delete;

	NetLink::SocketBindResult bind(const std::string &address, const int port, const NetLink::BindOptions &options) override;

	int sendTo(const std::string &address, const int port, const void *data, int size) override;
	int recvFrom(void *buffer, int maxSize, std::string &remoteAddress, int &remotePort, int timeoutMS) override;

	bool waitUntilReady(bool forReading, int timeoutMS) override;

	void close() override;

	bool isOpen() const override { return mSocket != INVALID_SOCKET; }
	int  getBoundPort() const override { return mBoundPort; }

private:
	void applyOptions(const NetLink::SocketBindOptions &options);

	WinSockScope mWinScope;
	SOCKET       mSocket    = INVALID_SOCKET;
	int          mBoundPort = 0;

	std::atomic<bool> mClosed = false;
};