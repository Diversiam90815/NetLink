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

#include "SocketTypes.h"


namespace netlink::net
{

class IDatagramSocket
{
public:
	virtual ~IDatagramSocket()																				 = default;

	virtual Result<size_t>	 sendTo(const SocketAddress &destination, std::span<const uint8_t> data)		 = 0;

	// Waits up to timeout for one datagram. Fails with SocketError::Timeout if none arrived.
	virtual Result<Datagram> receiveFrom(std::span<uint8_t> buffer, std::chrono::milliseconds timeout)		 = 0;

	virtual SocketAddress	 localAddress() const															 = 0;

	virtual void			 shutdown()																		 = 0;
};


// Creates a datagram socket bound to the given local address.
using DatagramSocketFactory = std::function<Result<std::unique_ptr<IDatagramSocket>>(const SocketAddress &localAddress, const BindOptions &options)>;

} // namespace netlink::net
