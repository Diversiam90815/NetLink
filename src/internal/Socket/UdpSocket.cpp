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
	auto native = platform::createSocket();
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


Result<size_t> UdpSocket::sendTo(const SocketAddress &destination, const std::span<const uint8_t> data)
{
	return platform::sendDatagram(mHandle.get(), destination, data);
}


Result<size_t> UdpSocket::sendParts(const SocketAddress &destination, const std::span<const uint8_t> head, const std::span<const uint8_t> body)
{
	return platform::sendDatagram(mHandle.get(), destination, head, body);
}


Result<Datagram> UdpSocket::receiveFrom(const std::span<uint8_t> buffer, const std::chrono::microseconds timeout)
{
	const auto deadline = SocketHandle::Clock::now() + timeout;

	while (true)
	{
		if (mHandle.isShutdown())
			return std::unexpected(SocketError::Closed);

		// Reading comes first: under load a datagram is already waiting, and the wait would only cost a second system call
		mHandle.noteReceiveAttempt();
		auto datagram = platform::receiveDatagram(mHandle.get(), buffer);

		if (datagram || datagram.error() != SocketError::WouldBlock)
			return datagram;

		if (timeout <= std::chrono::microseconds::zero())
			return std::unexpected(SocketError::Timeout);

		if (auto ready = mHandle.waitReadable(deadline); !ready)
			return std::unexpected(ready.error());
	}
}


Result<void> UdpSocket::waitReadable(const std::chrono::microseconds timeout)
{
	return mHandle.waitReadable(SocketHandle::Clock::now() + timeout);
}

} // namespace netlink::net
