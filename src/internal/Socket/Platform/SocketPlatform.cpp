/*
==============================================================================
	Module:         SocketPlatform
	Description:    BSD-portable part of the native socket abstraction
  ==============================================================================
*/

#include "SocketPlatform.h"
#include "SocketCommon.h"

#include <algorithm>
#include <limits>


namespace netlink::net::platform
{

using namespace common;


namespace
{

std::unexpected<SocketError> failure()
{
	return std::unexpected(lastError());
}


// An IPv4Address is always well formed, so this cannot fail. It keeps returning
// Result<> so the call sites stay uniform with the rest of the platform layer.
Result<sockaddr_in> toSockaddr(const SocketAddress &address)
{
	sockaddr_in addr{};
	addr.sin_family		 = AF_INET;
	addr.sin_port		 = htons(address.port);
	addr.sin_addr.s_addr = htonl(address.ip.toHostOrder()); // 0.0.0.0 maps to INADDR_ANY

	return addr;
}


SocketAddress fromSockaddr(const sockaddr_in &addr)
{
	return {IPv4Address::fromHostOrder(ntohl(addr.sin_addr.s_addr)), ntohs(addr.sin_port)};
}


Result<void> setIntOption(NativeHandle handle, int level, int name, int value)
{
	if (setsockopt(toNative(handle), level, name, reinterpret_cast<const char *>(&value), sizeof(value)) == SocketErrorRet)
		return failure();

	return {};
}


BufferLength clampLength(size_t size)
{
#if defined(_WIN32)
	return static_cast<BufferLength>(std::min<size_t>(size, static_cast<size_t>(std::numeric_limits<int>::max())));
#else
	return size;
#endif
}

} // namespace


Result<void> applyBindOptions(NativeHandle handle, const BindOptions &options)
{
	if (options.reuseAddress)
	{
		if (auto result = setIntOption(handle, SOL_SOCKET, SO_REUSEADDR, 1); !result)
			return result;

#if defined(SO_REUSEPORT)
		// BSD/macOS & Linux require SO_REUSEPORT for several sockets to share a UDP port
		if (auto result = setIntOption(handle, SOL_SOCKET, SO_REUSEPORT, 1); !result)
			return result;
#endif
	}

	if (options.enableBroadcast)
	{
		if (auto result = setIntOption(handle, SOL_SOCKET, SO_BROADCAST, 1); !result)
			return result;
	}

	if (options.receiveBufferSize > 0)
	{
		if (auto result = setIntOption(handle, SOL_SOCKET, SO_RCVBUF, options.receiveBufferSize); !result)
			return result;
	}

	if (options.sendBufferSize > 0)
	{
		if (auto result = setIntOption(handle, SOL_SOCKET, SO_SNDBUF, options.sendBufferSize); !result)
			return result;
	}

	return {};
}


Result<void> bindTo(NativeHandle handle, const SocketAddress &address)
{
	auto addr = toSockaddr(address);
	if (!addr)
		return std::unexpected(addr.error());

	if (::bind(toNative(handle), reinterpret_cast<sockaddr *>(&*addr), sizeof(sockaddr_in)) == SocketErrorRet)
		return failure();

	return {};
}


Result<SocketAddress> localAddressOf(NativeHandle handle)
{
	sockaddr_in addr{};
	SockLen		length = sizeof(addr);

	if (getsockname(toNative(handle), reinterpret_cast<sockaddr *>(&addr), &length) == SocketErrorRet)
		return failure();

	return fromSockaddr(addr);
}


Result<size_t> sendDatagram(NativeHandle handle, const SocketAddress &to, std::span<const uint8_t> data)
{
	auto addr = toSockaddr(to);
	if (!addr)
		return std::unexpected(addr.error());

	while (true)
	{
		const auto sent =
			::sendto(toNative(handle), reinterpret_cast<const char *>(data.data()), clampLength(data.size()), SendFlags, reinterpret_cast<sockaddr *>(&*addr), sizeof(sockaddr_in));

		if (sent != SocketErrorRet)
			return static_cast<size_t>(sent);

		const int code = lastNativeError();
		if (isInterrupted(code))
			continue;

		return std::unexpected(isWouldBlock(code) ? SocketError::WouldBlock : mapNativeError(code));
	}
}


Result<Datagram> receiveDatagram(NativeHandle handle, std::span<uint8_t> buffer)
{
	while (true)
	{
		sockaddr_in from{};
		SockLen		length	 = sizeof(from);

		const auto	received = ::recvfrom(toNative(handle), reinterpret_cast<char *>(buffer.data()), clampLength(buffer.size()), 0, reinterpret_cast<sockaddr *>(&from), &length);

		if (received != SocketErrorRet)
			return Datagram{static_cast<size_t>(received), fromSockaddr(from)};

		const int code = lastNativeError();
		if (isInterrupted(code))
			continue;

		return std::unexpected(isWouldBlock(code) ? SocketError::WouldBlock : mapNativeError(code));
	}
}


SocketError lastError()
{
	return mapNativeError(lastNativeError());
}

} // namespace netlink::net::platform
