/*
  ==============================================================================
	Module:         TCPServer
	Description:    Accepts inbound TCP connections and wraps them in TCPSessions
  ==============================================================================
*/

#include "TCPServer.h"

#include "NetLinkConstants.h"
#include "NetLinkLog.h"
#include "Socket/TcpListener.h"
#include "TCPSession.h"
#include "Util/ThreadUtils.h"


namespace netlink
{

// Shared with the accept thread, so stopping from inside the session handler cannot destroy what the thread still uses.
struct TCPServer::AcceptState
{
	AcceptState(net::TcpListener l, SessionHandler h) : listener(std::move(l)), handler(std::move(h)) {}

	net::TcpListener  listener;
	SessionHandler	  handler;
	std::atomic<bool> stopRequested{false};
};


TCPServer::~TCPServer()
{
	stop();
}


void TCPServer::setSessionHandler(SessionHandler handler)
{
	std::lock_guard<std::mutex> lock(mMutex);
	mSessionHandler = std::move(handler);
}


bool TCPServer::start(const std::string &localAddress)
{
	stop();

	auto listener = net::TcpListener::listen({localAddress, 0});

	if (!listener)
	{
		NETLINK_LOG_ERROR("TCPServer: listening on {} failed: {}", localAddress, net::toString(listener.error()));
		return false;
	}

	std::lock_guard<std::mutex> lock(mMutex);

	mBoundPort.store(listener->localAddress().port);
	NETLINK_LOG_INFO("TCPServer listening on {}", listener->localAddress().toString());

	mState	= std::make_shared<AcceptState>(std::move(*listener), mSessionHandler);
	mThread = std::thread([state = mState]() { acceptLoop(state); });
	return true;
}


void TCPServer::stop()
{
	std::thread thread;

	{
		std::lock_guard<std::mutex> lock(mMutex);

		if (!mState)
			return;

		mState->stopRequested.store(true);
		mState->listener.shutdown();
		mState.reset();

		thread = std::move(mThread);
	}

	joinOrDetach(thread);
	mBoundPort.store(0);
}


void TCPServer::acceptLoop(const std::shared_ptr<AcceptState> &state)
{
	while (!state->stopRequested.load())
	{
		auto stream = state->listener.accept(internal::SocketPollInterval);

		if (!stream)
		{
			if (stream.error() == net::SocketError::Timeout)
				continue;

			if (state->stopRequested.load())
				return;

			NETLINK_LOG_ERROR("TCPServer: accept failed: {}", net::toString(stream.error()));

			// Listener is unusable (e.g. network interface went away)
			return;
		}

		NETLINK_LOG_INFO("TCPServer: accepted connection from {}", stream->remoteAddress().toString());

		auto session = std::make_shared<TCPSession>(std::move(*stream));

		if (state->handler)
			state->handler(std::move(session));
	}
}

} // namespace netlink
