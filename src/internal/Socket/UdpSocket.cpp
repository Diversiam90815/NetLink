/*
==============================================================================
	Module:         UdpSocket
	Description:    Bound UDP socket
  ==============================================================================
*/

#include "UdpSocket.h"
#include "Platform/SocketPlatform.h"


namespace netlink::net
{

UdpSocket::UdpSocket(SocketHandle handle, SocketAddress localAddress) : mHandle(std::move(handle)), mLocalAddress(std::move(localAddress)) {}


Result<UdpSocket> UdpSocket::bind(const SocketAddress &localAddress, const BindOptions &options)
{
	auto native = platform::createSocket(platform::SocketKind::Datagram);
	if (!native)
		return std::unexpected(native.error());

	SocketHandle handle(*native);

	if (auto result = platform::applyBindOptions(handle.get(), options); !result)
		return std::unexpected(result.error());

	if (auto result = platform::bindTo(handle.get(), localAddress); !result)
		return std::unexpected(result.error());

	auto bound = platform::localAddressOf(handle.get());
	if (!bound)
		return std::unexpected(bound.error());

	return UdpSocket(std::move(handle), *bound);
}


DatagramSocketFactory UdpSocket::factory()
{
	return [](const SocketAddress &localAddress, const BindOptions &options) -> Result<std::unique_ptr<IDatagramSocket>>
	{
		auto socket = UdpSocket::bind(localAddress, options);
		if (!socket)
			return std::unexpected(socket.error());

		return std::make_unique<UdpSocket>(std::move(*socket));
	};
}


Result<size_t> UdpSocket::sendTo(const SocketAddress &destination, std::span<const uint8_t> data)
{
	return platform::sendDatagram(mHandle.get(), destination, data);
}


Result<Datagram> UdpSocket::receiveFrom(std::span<uint8_t> buffer, std::chrono::milliseconds timeout)
{
	if (auto ready = mHandle.wait(WaitFor::Readable, timeout); !ready)
		return std::unexpected(ready.error());

	auto datagram = platform::receiveDatagram(mHandle.get(), buffer);

	if (!datagram && datagram.error() == SocketError::WouldBlock)
		return std::unexpected(SocketError::Timeout);

	return datagram;
}

} // namespace netlink::net
