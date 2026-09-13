/*
==============================================================================
	Module:         UdpSocket
	Description:    Bound UDP socket
  ==============================================================================
*/

#pragma once

#include "IDatagramSocket.h"
#include "SocketHandle.h"


namespace netlink::net
{

class UdpSocket final : public IDatagramSocket
{
public:
	static Result<UdpSocket>	 bind(const SocketAddress &localAddress, const BindOptions &options = {});

	// Factory producing real UDP sockets
	static DatagramSocketFactory factory();

	UdpSocket(UdpSocket &&) noexcept				  = default;
	UdpSocket		&operator=(UdpSocket &&) noexcept = default;

	Result<size_t>	 sendTo(const SocketAddress &destination, std::span<const uint8_t> data) override;
	Result<Datagram> receiveFrom(std::span<uint8_t> buffer, std::chrono::milliseconds timeout) override;

	SocketAddress	 localAddress() const override { return mLocalAddress; }

	void			 shutdown() override { mHandle.shutdown(); }

private:
	UdpSocket(SocketHandle handle, SocketAddress localAddress);

	SocketHandle  mHandle;
	SocketAddress mLocalAddress;
};

} // namespace netlink::net
