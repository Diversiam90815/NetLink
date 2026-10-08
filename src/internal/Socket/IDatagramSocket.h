/*
==============================================================================
	Module:         IDatagramSocket
	Description:    Narrow datagram interface
  ==============================================================================
*/

#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "SocketTypes.h"


namespace netlink::net
{

class IDatagramSocket
{
public:
	virtual ~IDatagramSocket()																	   = default;

	virtual Result<size_t> sendTo(const SocketAddress &destination, std::span<const uint8_t> data) = 0;

	// Sends head and body as one datagram
	virtual Result<size_t> sendParts(const SocketAddress &destination, std::span<const uint8_t> head, std::span<const uint8_t> body)
	{
		std::vector<uint8_t> datagram;
		datagram.reserve(head.size() + body.size());
		datagram.insert(datagram.end(), head.begin(), head.end());
		datagram.insert(datagram.end(), body.begin(), body.end());
		return sendTo(destination, datagram);
	}

	// Waits up to timeout for one datagram. Fails with SocketError::Timeout if none arrived, and with
	// SocketError::Cancelled if interrupt() ended the wait.
	virtual Result<Datagram> receiveFrom(std::span<uint8_t> buffer, std::chrono::microseconds timeout) = 0;

	// Waits up to timeout until a datagram can be read, without reading it. Fails like receiveFrom().
	virtual Result<void>	 waitReadable(std::chrono::microseconds timeout)						   = 0;

	// Ends the wait another thread is in, or the next wait if none is in progress
	virtual void			 interrupt()															   = 0;

	virtual SocketAddress	 localAddress() const													   = 0;

	virtual void			 shutdown()																   = 0;
};


// Creates a datagram socket bound to the given local address.
using DatagramSocketFactory = std::function<Result<std::unique_ptr<IDatagramSocket>>(const SocketAddress &localAddress, const BindOptions &options)>;

} // namespace netlink::net
