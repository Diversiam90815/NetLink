/*
  ==============================================================================
	Module:         TCPClient
	Description:    Connects to a remote TCP server and provides a TCPSession
  ==============================================================================
*/

#include "TCPClient.h"

#include "NetLinkConstants.h"
#include "NetLinkLog.h"
#include "Socket/TcpStream.h"
#include "TCPSession.h"
#include "Util/ThreadUtils.h"


namespace netlink
{

TCPClient::~TCPClient()
{
	cancel();
}


void TCPClient::setConnectHandler(ConnectHandler handler)
{
	std::lock_guard<std::mutex> lock(mMutex);
	mConnectHandler = std::move(handler);
}


void TCPClient::setConnectTimeoutHandler(ConnectTimeoutHandler handler)
{
	std::lock_guard<std::mutex> lock(mMutex);
	mConnectTimeoutHandler = std::move(handler);
}


void TCPClient::connect(const std::string &host, unsigned short port)
{
	cancel();

	std::lock_guard<std::mutex> lock(mMutex);

	auto						cancelled = std::make_shared<std::atomic<bool>>(false);
	mCancelled							  = cancelled;

	// The thread owns copies of everything it uses, so it never touches this object
	mThread								  = std::thread(
		[cancelled, remote = net::SocketAddress{host, port}, onConnected = mConnectHandler, onFailed = mConnectTimeoutHandler]()
		{
			auto stream = net::TcpStream::connect(remote, internal::TcpConnectTimeout, [&cancelled]() { return cancelled->load(); });

			if (cancelled->load())
				return;

			if (!stream)
			{
				NETLINK_LOG_ERROR("TCPClient: connecting to {} failed: {}", remote.toString(), net::toString(stream.error()));

				if (onFailed)
					onFailed();
				return;
			}

			NETLINK_LOG_INFO("TCPClient: connected to {}", remote.toString());

			if (onConnected)
				onConnected(std::make_shared<TCPSession>(std::move(*stream)));
		});
}


void TCPClient::cancel()
{
	std::thread thread;

	{
		std::lock_guard<std::mutex> lock(mMutex);

		if (mCancelled)
			mCancelled->store(true);

		thread = std::move(mThread);
	}

	joinOrDetach(thread);
}

} // namespace netlink
