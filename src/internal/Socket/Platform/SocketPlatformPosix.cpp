/*
==============================================================================
	Module:         SocketPlatformPosix
	Description:    POSIX (Linux / macOS) specific part of the native socket abstraction
  ==============================================================================
*/

#include "SocketPlatform.h"
#include "SocketCommon.h"

#include <algorithm>
#include <ctime>

#if defined(__linux__)
#include <sys/eventfd.h>
#else
#include <sys/event.h>
#endif


namespace netlink::net::platform
{

using namespace common;


SocketError common::mapNativeError(const int code)
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
	case EHOSTDOWN: return SocketError::NetworkUnreachable;
	case ENETDOWN: return SocketError::NetworkDown;
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


Result<NativeHandle> createSocket()
{
	int type = SOCK_DGRAM;
#if defined(SOCK_CLOEXEC)
	type |= SOCK_CLOEXEC;
#endif

	int sock = ::socket(AF_INET, type, IPPROTO_UDP);

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


Result<void> setNonBlocking(const NativeHandle handle)
{
	const int flags = ::fcntl(toNative(handle), F_GETFL, 0);
	if (flags < 0 || ::fcntl(toNative(handle), F_SETFL, flags | O_NONBLOCK) < 0)
		return std::unexpected(lastError());

	return {};
}


void closeHandle(const NativeHandle handle)
{
	if (handle != InvalidNativeHandle)
		::close(toNative(handle));
}


void shutdownHandle(const NativeHandle handle)
{
	if (handle != InvalidNativeHandle)
		::shutdown(toNative(handle), SHUT_RDWR);
}


namespace
{

// Time left until the deadline, never negative
timespec remainingUntil(const std::chrono::steady_clock::time_point deadline)
{
	const auto remaining = std::max(std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - std::chrono::steady_clock::now()), std::chrono::nanoseconds{0});

	timespec   left{};
	left.tv_sec	 = static_cast<time_t>(remaining.count() / 1'000'000'000);
	left.tv_nsec = static_cast<long>(remaining.count() % 1'000'000'000);
	return left;
}


#if defined(__linux__)

class ReadWaiterLinux final : public ReadWaiter
{
public:
	explicit ReadWaiterLinux(const NativeSocket socket) : mSocket(socket), mInterrupt(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {}

	~ReadWaiterLinux() override
	{
		if (mInterrupt >= 0)
			::close(mInterrupt);
	}

	bool		 isValid() const { return mInterrupt >= 0; }

	Result<void> wait(const std::chrono::steady_clock::time_point deadline) override
	{
		while (true)
		{
			pollfd fds[2]{};
			fds[0].fd	  = mSocket;
			fds[0].events = POLLIN;
			fds[1].fd	  = mInterrupt;
			fds[1].events = POLLIN;

			const timespec timeout = remainingUntil(deadline);
			const int	   result  = ::ppoll(fds, 2, &timeout, nullptr);

			if (result == 0)
				return std::unexpected(SocketError::Timeout);

			if (result < 0)
			{
				if (errno == EINTR)
					continue;

				return std::unexpected(lastError());
			}

			if (fds[1].revents != 0)
			{
				// One interrupt() ends one wait
				uint64_t   count = 0;
				const auto ignored = ::read(mInterrupt, &count, sizeof(count));
				static_cast<void>(ignored);
				return std::unexpected(SocketError::Cancelled);
			}

			return {}; // readable, or POLLERR/POLLHUP which the following call will report
		}
	}

	void interrupt() override
	{
		const uint64_t one	   = 1;
		const auto	   ignored = ::write(mInterrupt, &one, sizeof(one));
		static_cast<void>(ignored);
	}

private:
	NativeSocket mSocket;
	int			 mInterrupt; // eventfd, readable after interrupt()
};

using PlatformReadWaiter = ReadWaiterLinux;

#else

class ReadWaiterKqueue final : public ReadWaiter
{
public:
	explicit ReadWaiterKqueue(const NativeSocket socket) : mQueue(::kqueue())
	{
		if (mQueue < 0)
			return;

		struct kevent changes[2];
		EV_SET(&changes[0], static_cast<uintptr_t>(socket), EVFILT_READ, EV_ADD, 0, 0, nullptr);
		EV_SET(&changes[1], InterruptIdent, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);

		if (::kevent(mQueue, changes, 2, nullptr, 0, nullptr) < 0)
		{
			::close(mQueue);
			mQueue = -1;
		}
	}

	~ReadWaiterKqueue() override
	{
		if (mQueue >= 0)
			::close(mQueue);
	}

	bool		 isValid() const { return mQueue >= 0; }

	Result<void> wait(const std::chrono::steady_clock::time_point deadline) override
	{
		while (true)
		{
			struct kevent  events[2];
			const timespec timeout = remainingUntil(deadline);
			const int	   count   = ::kevent(mQueue, nullptr, 0, events, 2, &timeout);

			if (count == 0)
				return std::unexpected(SocketError::Timeout);

			if (count < 0)
			{
				if (errno == EINTR)
					continue;

				return std::unexpected(lastError());
			}

			// EV_CLEAR resets the interrupt once it was reported: one interrupt() ends one wait
			for (int i = 0; i < count; ++i)
			{
				if (events[i].filter == EVFILT_USER)
					return std::unexpected(SocketError::Cancelled);
			}

			return {}; // readable, or an error condition the following call will report
		}
	}

	void interrupt() override
	{
		struct kevent trigger;
		EV_SET(&trigger, InterruptIdent, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
		::kevent(mQueue, &trigger, 1, nullptr, 0, nullptr);
	}

private:
	static constexpr uintptr_t InterruptIdent = 1; // identifiers are per filter: no clash with a descriptor of the same value

	int						   mQueue;
};

using PlatformReadWaiter = ReadWaiterKqueue;

#endif

} // namespace


std::unique_ptr<ReadWaiter> createReadWaiter(const NativeHandle handle)
{
	auto waiter = std::make_unique<PlatformReadWaiter>(toNative(handle));
	return waiter->isValid() ? std::move(waiter) : nullptr;
}

} // namespace netlink::net::platform
