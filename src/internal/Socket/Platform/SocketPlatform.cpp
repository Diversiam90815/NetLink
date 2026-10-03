/*
==============================================================================
	Module:         SocketPlatform
	Description:    BSD-portable part of the native socket abstraction
  ==============================================================================
*/

#include "SocketPlatform.h"
#include "SocketCommon.h"
#include "NetLinkLog.h"

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
	return {.ip = IPv4Address::fromHostOrder(ntohl(addr.sin_addr.s_addr)), .port = ntohs(addr.sin_port)};
}


Result<void> setIntOption(const NativeHandle handle, const int level, const int name, const int value)
{
	if (setsockopt(toNative(handle), level, name, reinterpret_cast<const char *>(&value), sizeof(value)) == SocketErrorRet)
		return failure();

	return {};
}


BufferLength clampLength(const size_t size)
{
#if defined(_WIN32)
	return static_cast<BufferLength>(std::min<size_t>(size, static_cast<size_t>(std::numeric_limits<int>::max())));
#else
	return size;
#endif
}

} // namespace


Result<void> applyBindOptions(const NativeHandle handle, const BindOptions &options)
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

	// Buffer sizes are hints: the OS may cap them (Linux: net.core.rmem_max / wmem_max), so a refusal does not fail the socket
	if (options.receiveBufferSize > 0)
	{
		if (auto result = setIntOption(handle, SOL_SOCKET, SO_RCVBUF, options.receiveBufferSize); !result)
			NETLINK_LOG_WARNING("Receive buffer of {} bytes not applied: {}", options.receiveBufferSize, toString(result.error()));
	}

	if (options.sendBufferSize > 0)
	{
		if (auto result = setIntOption(handle, SOL_SOCKET, SO_SNDBUF, options.sendBufferSize); !result)
			NETLINK_LOG_WARNING("Send buffer of {} bytes not applied: {}", options.sendBufferSize, toString(result.error()));
	}

	return {};
}


Result<void> bindTo(const NativeHandle handle, const SocketAddress &address)
{
	auto addr = toSockaddr(address);
	if (!addr)
		return std::unexpected(addr.error());

	if (::bind(toNative(handle), reinterpret_cast<sockaddr *>(&*addr), sizeof(sockaddr_in)) == SocketErrorRet)
		return failure();

	return {};
}


Result<SocketAddress> localAddressOf(const NativeHandle handle)
{
	sockaddr_in addr{};
	SockLen		length = sizeof(addr);

	if (getsockname(toNative(handle), reinterpret_cast<sockaddr *>(&addr), &length) == SocketErrorRet)
		return failure();

	return fromSockaddr(addr);
}


Result<size_t> sendDatagram(const NativeHandle handle, const SocketAddress &to, const std::span<const uint8_t> data)
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


Result<size_t> sendDatagram(const NativeHandle handle, const SocketAddress &to, const std::span<const uint8_t> head, const std::span<const uint8_t> body)
{
	auto addr = toSockaddr(to);
	if (!addr)
		return std::unexpected(addr.error());

	while (true)
	{
#if defined(_WIN32)
		WSABUF buffers[2];
		buffers[0].buf = const_cast<char *>(reinterpret_cast<const char *>(head.data()));
		buffers[0].len = static_cast<ULONG>(head.size());
		buffers[1].buf = const_cast<char *>(reinterpret_cast<const char *>(body.data()));
		buffers[1].len = static_cast<ULONG>(body.size());

		DWORD	   sent	  = 0;
		const bool failed = WSASendTo(toNative(handle), buffers, body.empty() ? 1 : 2, &sent, 0, reinterpret_cast<sockaddr *>(&*addr), sizeof(sockaddr_in), nullptr, nullptr) == SOCKET_ERROR;
#else
		iovec parts[2];
		parts[0].iov_base = const_cast<uint8_t *>(head.data());
		parts[0].iov_len  = head.size();
		parts[1].iov_base = const_cast<uint8_t *>(body.data());
		parts[1].iov_len  = body.size();

		msghdr message{};
		message.msg_name	= &*addr;
		message.msg_namelen = sizeof(sockaddr_in);
		message.msg_iov		= parts;
		message.msg_iovlen	= body.empty() ? 1 : 2;

		const auto result = ::sendmsg(toNative(handle), &message, SendFlags);
		const bool failed = result == SocketErrorRet;
		const auto sent	  = result;
#endif

		if (!failed)
			return static_cast<size_t>(sent);

		const int code = lastNativeError();
		if (isInterrupted(code))
			continue;

		return std::unexpected(isWouldBlock(code) ? SocketError::WouldBlock : mapNativeError(code));
	}
}


Result<Datagram> receiveDatagram(const NativeHandle handle, std::span<uint8_t> buffer)
{
	while (true)
	{
		sockaddr_in from{};

#if defined(_WIN32)
		// A datagram that does not fit into the buffer fails with WSAEMSGSIZE, and is gone
		SockLen	   length	= sizeof(from);
		const auto received = ::recvfrom(toNative(handle), reinterpret_cast<char *>(buffer.data()), clampLength(buffer.size()), 0, reinterpret_cast<sockaddr *>(&from), &length);
#else
		iovec part{};
		part.iov_base = buffer.data();
		part.iov_len  = buffer.size();

		msghdr message{};
		message.msg_name	= &from;
		message.msg_namelen = sizeof(from);
		message.msg_iov		= &part;
		message.msg_iovlen	= 1;

		const auto received = ::recvmsg(toNative(handle), &message, 0);

		// A datagram that does not fit into the buffer is cut off silently, and its rest is gone: reported like on Windows
		if (received != SocketErrorRet && (message.msg_flags & MSG_TRUNC) != 0)
			return std::unexpected(SocketError::MessageTooLarge);
#endif

		if (received != SocketErrorRet)
			return Datagram{.size = static_cast<size_t>(received), .from = fromSockaddr(from)};

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
