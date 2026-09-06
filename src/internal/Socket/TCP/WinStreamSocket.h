/*
==============================================================================
	Module:         WinStreamSocket
	Description:    Low-level WinSock2 TCP socket
  ==============================================================================
*/

#pragma once

#include <atomic>
#include <string>

#include "../Common/WinsockCommon.h"


class WinStreamSocket : public ISocket
{
public:
	WinStreamSocket() = default;
	~WinStreamSocket() override;
	WinStreamSocket(const WinStreamSocket &)					 = delete;
	WinStreamSocket			 &operator=(const WinStreamSocket &) = delete;

	NetLink::SocketBindResult bind(const std::string &address, const int port, const NetLink::BindOptions &options) override;

	bool					  listen(int backlog) override;
	bool					  accept(int timeoutMS) override;
	std::unique_ptr<ISocket>  acceptNew(int timeoutMS) override;
	bool					  connect(const std::string &address, int port, int timeoutMS) override;
	bool					  isConnected() const override { return mConnected != INVALID_SOCKET; }

	// address/port arguments are ignored for TCP (connection already fixed)
	int						  sendTo(const std::string &address, const int port, const void *data, int size) override;
	int						  recvFrom(void *buffer, int maxSize, std::string &remoteAddress, int &remotePort, int timeoutMS) override;

	bool					  waitUntilReady(bool forReading, int timeoutMS) override;

	void					  close() override;

	bool					  isOpen() const override { return mSocket != INVALID_SOCKET || mConnected != INVALID_SOCKET; }
	int						  getBoundPort() const override { return mBoundPort; }
	std::string				  getRemoteAddress() const override { return mRemoteAddress; }
	int						  getRemotePort() const override { return mRemotePort; }

private:
	void			  applyOptions(SOCKET sock, const NetLink::BindOptions &options);
	bool			  waitOn(SOCKET sock, bool forReading, int timeoutMS) const;


	WinsockScope	  mWinScope;

	SOCKET			  mSocket	 = INVALID_SOCKET; // listening (server) or connecting (client) socket
	SOCKET			  mConnected = INVALID_SOCKET; // accepted peer (server) / == mSocket after connect (client)
	int				  mBoundPort = 0;

	std::string		  mRemoteAddress;
	int				  mRemotePort = 0;

	std::atomic<bool> mClosed	  = false;
};
