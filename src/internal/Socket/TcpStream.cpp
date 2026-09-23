/*
==============================================================================
	Module:         TcpStream
	Description:    Connected TCP byte stream
  ==============================================================================
*/

#include "TcpStream.h"
#include "Platform/SocketPlatform.h"
#include "NetLinkLog.h"

#include <algorithm>


namespace netlink::net
{

namespace
{
constexpr std::chrono::milliseconds CancelPollInterval{50};
}


TcpStream::TcpStream(SocketHandle handle, SocketAddress localAddress, SocketAddress remoteAddress)
	: mHandle(std::move(handle)), mLocalAddress(std::move(localAddress)), mRemoteAddress(std::move(remoteAddress))
{
}


Result<TcpStream> TcpStream::connect(const SocketAddress &remote, std::chrono::milliseconds timeout, const CancelPredicate &isCancelled, const SocketAddress &localAddress)
{
	using Clock = std::chrono::steady_clock;

	auto native = platform::createSocket(platform::SocketKind::Stream);
	if (!native)
		return std::unexpected(native.error());

	SocketHandle handle(*native);

	if (!localAddress.ip.isUnspecified())
	{
		if (auto bound = platform::bindTo(handle.get(), localAddress); !bound)
			return std::unexpected(bound.error());
	}

	auto started = platform::startConnect(handle.get(), remote);

	if (!started && started.error() != SocketError::WouldBlock)
		return std::unexpected(started.error());

	if (!started)
	{
		const auto deadline = Clock::now() + timeout;

		while (true)
		{
			if (isCancelled && isCancelled())
				return std::unexpected(SocketError::Cancelled);

			const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
			if (remaining.count() <= 0)
				return std::unexpected(SocketError::Timeout);

			auto ready = handle.wait(WaitFor::Writable, std::min(remaining, CancelPollInterval));

			if (ready)
				break;

			if (ready.error() != SocketError::Timeout)
				return std::unexpected(ready.error());
		}

		if (auto result = platform::finishConnect(handle.get()); !result)
			return std::unexpected(result.error());
	}

	if (auto noDelay = platform::setNoDelay(handle.get()); !noDelay)
		NETLINK_LOG_WARNING("TCP_NODELAY could not be set: {}", toString(noDelay.error()));

	auto local = platform::localAddressOf(handle.get());
	if (!local)
		return std::unexpected(local.error());

	return TcpStream(std::move(handle), *local, remote);
}


Result<void> TcpStream::sendAll(std::span<const uint8_t> data, std::chrono::milliseconds timeout)
{
	using Clock			= std::chrono::steady_clock;
	const auto deadline = Clock::now() + timeout;

	while (!data.empty())
	{
		auto sent = platform::sendSome(mHandle.get(), data);

		if (sent)
		{
			data = data.subspan(*sent);
			continue;
		}

		if (sent.error() != SocketError::WouldBlock)
			return std::unexpected(sent.error());

		const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
		if (remaining.count() <= 0)
			return std::unexpected(SocketError::Timeout);

		if (auto ready = mHandle.wait(WaitFor::Writable, remaining); !ready)
			return std::unexpected(ready.error());
	}

	return {};
}


Result<size_t> TcpStream::receiveSome(std::span<uint8_t> buffer, std::chrono::milliseconds timeout)
{
	if (auto ready = mHandle.wait(WaitFor::Readable, timeout); !ready)
		return std::unexpected(ready.error());

	auto received = platform::receiveSome(mHandle.get(), buffer);

	// Readiness can be spurious
	if (!received && received.error() == SocketError::WouldBlock)
		return std::unexpected(SocketError::Timeout);

	return received;
}

} // namespace netlink::net
