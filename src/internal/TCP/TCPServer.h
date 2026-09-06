/*
  ==============================================================================
	Module:         TCPServer
	Description:    Server implementation for TCP Connections
  ==============================================================================
*/

#pragma once

#include <memory>

#include "../Util/ThreadBase.h"
#include "../Socket/NetlinkSocket.h"
#include "Transport/TransportInterfaces.h"
#include "TCPSession.h"


// Implements a TCP server that listens for and accepts incoming connections, TCPSession instances for each accepted socket.
class TCPServer : public IServer, private ThreadBase
{
public:
	TCPServer() = default;
	~TCPServer() override;

	bool bindAndListen(const std::string &address, int port, int backlog = 8);

	void startAccept() override;
	void stopAccept();

	int	 getBoundPort() const override;

	void setSessionHandler(SessionHandler handler) override;

	void respondToConnectionRequest(bool accepted) override;

private:
	void						run() override;


	NetlinkSocket				mAcceptorSocket; // TCP, listening
	int							mBoundPort{0};

	std::shared_ptr<TCPSession> mPendingSession;
	SessionHandler				mSessionHandler;
};
