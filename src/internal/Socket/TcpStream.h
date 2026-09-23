/*
==============================================================================
	Module:         TcpStream
	Description:    Connected TCP byte stream
  ==============================================================================
*/

#pragma once

#include <chrono>
#include <functional>
#include <span>

#include "SocketHandle.h"
#include "SocketTypes.h"


namespace netlink::net
{

class TcpListener;


// Threading contract: one thread may send while another receives.
class TcpStream
{
public:
	using CancelPredicate = std::function<bool()>;

	// Connects with a timeout. isCancelled is polled periodically so a pending connect can be aborted quickly.
	// A non-empty localAddress.ip binds the outgoing connection to that interface first.
	static Result<TcpStream> connect(const SocketAddress &remote, std::chrono::milliseconds timeout, const CancelPredicate &isCancelled = {}, const SocketAddress &localAddress = {});

	TcpStream(TcpStream &&) noexcept					  = default;
	TcpStream			&operator=(TcpStream &&) noexcept = default;

	// Sends the complete buffer or fails
	Result<void>		 sendAll(std::span<const uint8_t> data, std::chrono::milliseconds timeout);

	// Receives at least one byte. Fails with SocketError::Timeout if nothing arrived, SocketError::Closed on orderly shutdown.
	Result<size_t>		 receiveSome(std::span<uint8_t> buffer, std::chrono::milliseconds timeout);

	const SocketAddress &localAddress() const { return mLocalAddress; }
	const SocketAddress &remoteAddress() const { return mRemoteAddress; }

	void				 shutdown() const { mHandle.shutdown(); }

private:
	friend class TcpListener;

	TcpStream(SocketHandle handle, SocketAddress localAddress, SocketAddress remoteAddress);

	SocketHandle  mHandle;
	SocketAddress mLocalAddress;
	SocketAddress mRemoteAddress;
};

} // namespace netlink::net
