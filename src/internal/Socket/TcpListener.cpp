/*
==============================================================================
	Module:         TcpListener
	Description:    Listening TCP socket producing TcpStreams
  ==============================================================================
*/

#include "TcpListener.h"
#include "Platform/SocketPlatform.h"
#include "NetLinkLog.h"


namespace netlink::net
{

TcpListener::TcpListener(SocketHandle handle, SocketAddress localAddress) : mHandle(std::move(handle)), mLocalAddress(std::move(localAddress)) {}


Result<TcpListener> TcpListener::listen(const SocketAddress &localAddress, int backlog)
{
	auto native = platform::createSocket(platform::SocketKind::Stream);
	if (!native)
		return std::unexpected(native.error());

	SocketHandle handle(*native);

	if (auto result = platform::bindTo(handle.get(), localAddress); !result)
		return std::unexpected(result.error());

	if (auto result = platform::listenOn(handle.get(), backlog); !result)
		return std::unexpected(result.error());

	auto bound = platform::localAddressOf(handle.get());
	if (!bound)
		return std::unexpected(bound.error());

	return TcpListener(std::move(handle), *bound);
}


Result<TcpStream> TcpListener::accept(std::chrono::milliseconds timeout)
{
	if (auto ready = mHandle.wait(WaitFor::Readable, timeout); !ready)
		return std::unexpected(ready.error());

	auto accepted = platform::acceptOne(mHandle.get());

	if (!accepted)
	{
		// The client may have given up between readiness and accept()
		if (accepted.error() == SocketError::WouldBlock || accepted.error() == SocketError::ConnectionAborted)
			return std::unexpected(SocketError::Timeout);

		return std::unexpected(accepted.error());
	}

	SocketHandle handle(accepted->first);

	if (auto prepared = platform::prepareAcceptedSocket(handle.get()); !prepared)
		return std::unexpected(prepared.error());

	if (auto noDelay = platform::setNoDelay(handle.get()); !noDelay)
		NETLINK_LOG_WARNING("TCP_NODELAY could not be set: {}", toString(noDelay.error()));

	auto local = platform::localAddressOf(handle.get());
	if (!local)
		return std::unexpected(local.error());

	return TcpStream(std::move(handle), *local, accepted->second);
}

} // namespace netlink::net
