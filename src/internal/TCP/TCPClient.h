/*
  ==============================================================================
	Module:         TCPClient
	Description:    Connects to a remote TCP server and provides a TCPSession
  ==============================================================================
*/

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "Transport/TransportInterfaces.h"


namespace netlink
{

class TCPClient final : public IClient
{
public:
	TCPClient() = default;
	~TCPClient() override;
	TCPClient(const TCPClient &)			= delete;
	TCPClient &operator=(const TCPClient &) = delete;

	void	   setConnectHandler(ConnectHandler handler) override;
	void	   setConnectTimeoutHandler(ConnectTimeoutHandler handler) override;

	void	   connect(const net::IPv4Address &localAddress, const net::IPv4Address &host, unsigned short port) override;

private:
	// Cancels a pending attempt; returns once its handlers can no longer fire
	void							   cancel();

	std::mutex						   mMutex;
	ConnectHandler					   mConnectHandler;
	ConnectTimeoutHandler			   mConnectTimeoutHandler;

	std::thread						   mThread;
	std::shared_ptr<std::atomic<bool>> mCancelled;
};

} // namespace netlink
