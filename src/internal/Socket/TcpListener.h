/*
==============================================================================
	Module:         TcpListener
	Description:    Listening TCP socket producing TcpStreams
  ==============================================================================
*/

#pragma once

#include <chrono>

#include "SocketHandle.h"
#include "SocketTypes.h"
#include "TcpStream.h"


namespace netlink::net
{

// Threading contract: accept() runs on one thread; shutdown() may be called from any thread.
class TcpListener
{
public:
	// Binds and listens. Use port 0 to let the OS pick a free port
	static Result<TcpListener> listen(const SocketAddress &localAddress, int backlog = 8);

	TcpListener(TcpListener &&) noexcept					= default;
	TcpListener			&operator=(TcpListener &&) noexcept = default;

	// Waits up to timeout for an incoming connection
	Result<TcpStream>	 accept(std::chrono::milliseconds timeout);

	const SocketAddress &localAddress() const { return mLocalAddress; }

	void				 shutdown() const { mHandle.shutdown(); }

private:
	TcpListener(SocketHandle handle, SocketAddress localAddress);

	SocketHandle  mHandle;
	SocketAddress mLocalAddress;
};

} // namespace netlink::net
