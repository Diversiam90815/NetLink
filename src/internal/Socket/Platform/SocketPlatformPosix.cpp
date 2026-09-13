/*
==============================================================================
	Module:         SocketPlatformPosix
	Description:    POSIX (Linux / macOS) specific part of the native socket abstraction
  ==============================================================================
*/

#include "SocketPlatform.h"
#include "SocketCommon.h"

#include <algorithm>
#include <climits>


namespace netlink::net::platform
{

using namespace common;


SocketError common::mapNativeError(int code)
{
	switch (code)
	{
#if EAGAIN != EWOULDBLOCK
	case EWOULDBLOCK:
#endif
	case EAGAIN:
	case EINPROGRESS: return SocketError::WouldBlock;
	case ETIMEDOUT: return SocketError::Timeout;
	case EPIPE:
	case ESHUTDOWN: return SocketError::Closed;
	case ECONNREFUSED: return SocketError::ConnectionRefused;
	case ECONNRESET:
	case ENETRESET: return SocketError::ConnectionReset;
	case ECONNABORTED: return SocketError::ConnectionAborted;
	case ENOTCONN: return SocketError::NotConnected;
	case EADDRINUSE: return SocketError::AddressInUse;
	case EADDRNOTAVAIL: return SocketError::AddressNotAvailable;
	case EACCES:
	case EPERM: return SocketError::PermissionDenied;
	case EINVAL:
	case EFAULT:
	case EBADF:
	case ENOTSOCK:
	case EAFNOSUPPORT: return SocketError::InvalidArgument;
	case ENETUNREACH:
	case EHOSTUNREACH:
	case ENETDOWN: return SocketError::NetworkUnreachable;
	case EMSGSIZE: return SocketError::MessageTooLarge;
	default: return SocketError::Unknown;
	}
}


namespace
{

void disableSigPipe([[maybe_unused]] int sock)
{
#if defined(SO_NOSIGPIPE)
	const int noSigPipe = 1;
	::setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe));
#endif
}

} // namespace


Result<void> ensureInitialized()
{
	// Nothing to initialize: SIGPIPE is avoided per call (MSG_NOSIGNAL) or per socket (SO_NOSIGPIPE),
	// so the library never touches the host application's signal handlers.
	return {};
}


Result<NativeHandle> createSocket(SocketKind kind)
{
	const bool isStream = kind == SocketKind::Stream;

	int		   type		= isStream ? SOCK_STREAM : SOCK_DGRAM;
#if defined(SOCK_CLOEXEC)
	type |= SOCK_CLOEXEC;
#endif

	int sock = ::socket(AF_INET, type, isStream ? IPPROTO_TCP : IPPROTO_UDP);

	if (sock < 0)
		return std::unexpected(lastError());

#if !defined(SOCK_CLOEXEC)
	::fcntl(sock, F_SETFD, FD_CLOEXEC);
#endif

	if (auto nonBlocking = setNonBlocking(fromNative(sock)); !nonBlocking)
	{
		::close(sock);
		return std::unexpected(nonBlocking.error());
	}

	disableSigPipe(sock);

	return fromNative(sock);
}


Result<void> prepareAcceptedSocket(NativeHandle handle)
{
#if defined(FD_CLOEXEC)
	::fcntl(toNative(handle), F_SETFD, FD_CLOEXEC);
#endif
	disableSigPipe(toNative(handle));
	return setNonBlocking(handle);
}


Result<void> setNonBlocking(NativeHandle handle)
{
	const int flags = ::fcntl(toNative(handle), F_GETFL, 0);
	if (flags < 0 || ::fcntl(toNative(handle), F_SETFL, flags | O_NONBLOCK) < 0)
		return std::unexpected(lastError());

	return {};
}


void closeHandle(NativeHandle handle)
{
	if (handle != InvalidNativeHandle)
		::close(toNative(handle));
}


void shutdownHandle(NativeHandle handle)
{
	if (handle != InvalidNativeHandle)
		::shutdown(toNative(handle), SHUT_RDWR);
}


Result<void> waitUntil(NativeHandle handle, WaitFor what, std::chrono::milliseconds timeout)
{
	if (handle == InvalidNativeHandle)
		return std::unexpected(SocketError::InvalidArgument);

	using Clock			= std::chrono::steady_clock;
	const auto deadline = Clock::now() + std::max(timeout, std::chrono::milliseconds{0});

	pollfd	   fd{};
	fd.fd	  = toNative(handle);
	fd.events = what == WaitFor::Readable ? POLLIN : POLLOUT;

	while (true)
	{
		const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
		const int  waitMs	 = static_cast<int>(std::clamp<std::chrono::milliseconds::rep>(remaining.count(), 0, INT_MAX));

		const int  result	 = ::poll(&fd, 1, waitMs);

		if (result > 0)
			return {}; // ready, or POLLERR/POLLHUP which the following call will report

		if (result == 0)
			return std::unexpected(SocketError::Timeout);

		if (errno != EINTR)
			return std::unexpected(lastError());
	}
}

} // namespace netlink::net::platform
