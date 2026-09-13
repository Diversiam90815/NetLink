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


Result<sockaddr_in> toSockaddr(const SocketAddress &address)
{
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port	= htons(address.port);

	if (address.ip.empty())
	{
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
		return addr;
	}

	if (inet_pton(AF_INET, address.ip.c_str(), &addr.sin_addr) != 1)
		return std::unexpected(SocketError::InvalidArgument);

	return addr;
}


SocketAddress fromSockaddr(const sockaddr_in &addr)
{
	char text[INET_ADDRSTRLEN]{};
	inet_ntop(AF_INET, &addr.sin_addr, text, sizeof(text));
	return {text, ntohs(addr.sin_port)};
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


Result<void> setNoDelay(NativeHandle handle)
{
	return setIntOption(handle, IPPROTO_TCP, TCP_NODELAY, 1);
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


Result<void> listenOn(NativeHandle handle, int backlog)
{
	if (::listen(toNative(handle), backlog) == SocketErrorRet)
		return failure();

	return {};
}


Result<std::pair<NativeHandle, SocketAddress>> acceptOne(NativeHandle listener)
{
	sockaddr_in	 peer{};
	SockLen		 length	  = sizeof(peer);

	NativeSocket accepted = ::accept(toNative(listener), reinterpret_cast<sockaddr *>(&peer), &length);

	if (fromNative(accepted) == InvalidNativeHandle)
		return failure();

	return std::pair{fromNative(accepted), fromSockaddr(peer)};
}


Result<void> startConnect(NativeHandle handle, const SocketAddress &remote)
{
	auto addr = toSockaddr(remote);
	if (!addr)
		return std::unexpected(addr.error());

	if (remote.ip.empty() || remote.port == 0)
		return std::unexpected(SocketError::InvalidArgument);

	if (::connect(toNative(handle), reinterpret_cast<sockaddr *>(&*addr), sizeof(sockaddr_in)) == SocketErrorRet)
	{
		const int code = lastNativeError();

		if (isConnectInProgress(code))
			return std::unexpected(SocketError::WouldBlock);

		return std::unexpected(mapNativeError(code));
	}

	return {};
}


Result<void> finishConnect(NativeHandle handle)
{
	int		pendingError = 0;
	SockLen length		 = sizeof(pendingError);

	if (getsockopt(toNative(handle), SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&pendingError), &length) == SocketErrorRet)
		return failure();

	if (pendingError != 0)
		return std::unexpected(mapNativeError(pendingError));

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


Result<SocketAddress> remoteAddressOf(NativeHandle handle)
{
	sockaddr_in addr{};
	SockLen		length = sizeof(addr);

	if (getpeername(toNative(handle), reinterpret_cast<sockaddr *>(&addr), &length) == SocketErrorRet)
		return failure();

	return fromSockaddr(addr);
}


Result<size_t> sendSome(NativeHandle handle, std::span<const uint8_t> data)
{
	while (true)
	{
		const auto sent = ::send(toNative(handle), reinterpret_cast<const char *>(data.data()), clampLength(data.size()), SendFlags);

		if (sent != SocketErrorRet)
			return static_cast<size_t>(sent);

		const int code = lastNativeError();
		if (isInterrupted(code))
			continue;

		return std::unexpected(isWouldBlock(code) ? SocketError::WouldBlock : mapNativeError(code));
	}
}


Result<size_t> receiveSome(NativeHandle handle, std::span<uint8_t> buffer)
{
	while (true)
	{
		const auto received = ::recv(toNative(handle), reinterpret_cast<char *>(buffer.data()), clampLength(buffer.size()), 0);

		if (received == 0 && !buffer.empty())
			return std::unexpected(SocketError::Closed);

		if (received != SocketErrorRet)
			return static_cast<size_t>(received);

		const int code = lastNativeError();
		if (isInterrupted(code))
			continue;

		return std::unexpected(isWouldBlock(code) ? SocketError::WouldBlock : mapNativeError(code));
	}
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
