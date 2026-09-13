/*
  ==============================================================================
	Module:         TCPServer
	Description:    Accepts inbound TCP connections and wraps them in TCPSessions
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

class TCPServer final : public IServer
{
public:
	TCPServer() = default;
	~TCPServer() override;
	TCPServer(const TCPServer &)			= delete;
	TCPServer &operator=(const TCPServer &) = delete;

	void	   setSessionHandler(SessionHandler handler) override;

	bool	   start(const std::string &localAddress) override;
	void	   stop() override;

	int		   getBoundPort() const override { return mBoundPort.load(); }

private:
	struct AcceptState;

	static void					 acceptLoop(const std::shared_ptr<AcceptState> &state);

	std::mutex					 mMutex;
	std::shared_ptr<AcceptState> mState;
	std::thread					 mThread;
	SessionHandler				 mSessionHandler;
	std::atomic<int>			 mBoundPort{0};
};

} // namespace netlink
