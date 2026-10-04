/*
==============================================================================
	Module:         SocketPlatformWindows
	Description:    Winsock specific part of the native socket abstraction
  ==============================================================================
*/

#include "SocketPlatform.h"
#include "SocketCommon.h"
#include "Util/Timing/WaitableTimerWindows.h"

#include <atomic>


#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif


namespace netlink::net::platform
{

using namespace common;


namespace
{

// One WSAStartup per process, released when the process unloads the library.
class WinsockRuntime
{
public:
	WinsockRuntime()
	{
		WSADATA data{};
		mStartupResult = WSAStartup(MAKEWORD(2, 2), &data);
	}

	~WinsockRuntime()
	{
		if (mStartupResult == 0)
			WSACleanup();
	}

	WinsockRuntime(const WinsockRuntime &)			  = delete;
	WinsockRuntime &operator=(const WinsockRuntime &) = delete;

	bool			isInitialized() const { return mStartupResult == 0; }

private:
	int mStartupResult = -1;
};



class ReadWaiterWindows final : public ReadWaiter
{
public:
	explicit ReadWaiterWindows(const NativeSocket socket)
		: mSocket(socket), mReadable(CreateEventW(nullptr, FALSE, FALSE, nullptr)), mInterrupt(CreateEventW(nullptr, FALSE, FALSE, nullptr))
	{
		// Also puts the socket into non-blocking mode, which it already is
		if (mReadable && WSAEventSelect(mSocket, mReadable, FD_READ | FD_CLOSE) == SOCKET_ERROR)
		{
			CloseHandle(mReadable);
			mReadable = nullptr;
		}
	}

	~ReadWaiterWindows() override
	{
		if (mReadable)
		{
			WSAEventSelect(mSocket, nullptr, 0);
			CloseHandle(mReadable);
		}

		if (mInterrupt)
			CloseHandle(mInterrupt);
	}

	bool		 isValid() const { return mReadable != nullptr && mInterrupt != nullptr && mTimer.isValid(); }

	Result<void> wait(const std::chrono::steady_clock::time_point deadline) override
	{
		// Reported as readable before and nothing was read since: still readable. Winsock only signals again after a receive call.
		if (mReadableUnread.load())
			return {};

		// The interrupt comes first: it wins when several handles are signalled
		const HANDLE handles[] = {mInterrupt, mReadable, mTimer.handle()};

		while (true)
		{
			const auto now	   = std::chrono::steady_clock::now();
			const bool expired = now >= deadline;

			if (!expired)
				mTimer.armFor(deadline, now);

			switch (WaitForMultipleObjects(expired ? 2 : 3, handles, FALSE, expired ? 0 : INFINITE))
			{
			case WAIT_OBJECT_0: return std::unexpected(SocketError::Cancelled);

			// Winsock signals the event again as soon as a receive call leaves data behind or new data arrives. A wait may
			// therefore end although everything was read in the meantime: callers read without blocking and wait again.
			case WAIT_OBJECT_0 + 1:
				mReadableUnread.store(true);
				return {};

			case WAIT_OBJECT_0 + 2:
				mTimer.onSignalled(); // possibly for an earlier deadline: the clock decides
				continue;

			case WAIT_TIMEOUT: return std::unexpected(SocketError::Timeout);

			default: return std::unexpected(SocketError::Unknown);
			}
		}
	}

	void onReceiveAttempt() override { mReadableUnread.store(false); }

	void interrupt() override { SetEvent(mInterrupt); }

private:
	NativeSocket		  mSocket;
	HANDLE				  mReadable;  // auto-reset, signalled by Winsock when a datagram is waiting
	HANDLE				  mInterrupt; // auto-reset: one interrupt() ends one wait
	timing::WaitableTimer mTimer;
	std::atomic<bool>	  mReadableUnread{false};
};

} // namespace


SocketError common::mapNativeError(const int code)
{
	switch (code)
	{
	case WSAEWOULDBLOCK:
	case WSAEINPROGRESS: return SocketError::WouldBlock;
	case WSAETIMEDOUT: return SocketError::Timeout;
	case WSAESHUTDOWN:
	case WSAEDISCON: return SocketError::Closed;
	case WSAECONNREFUSED: return SocketError::ConnectionRefused;
	case WSAECONNRESET:
	case WSAENETRESET: return SocketError::ConnectionReset;
	case WSAECONNABORTED: return SocketError::ConnectionAborted;
	case WSAENOTCONN: return SocketError::NotConnected;
	case WSAEADDRINUSE: return SocketError::AddressInUse;
	case WSAEADDRNOTAVAIL: return SocketError::AddressNotAvailable;
	case WSAEACCES: return SocketError::PermissionDenied;
	case WSAEINVAL:
	case WSAEFAULT:
	case WSAENOTSOCK:
	case WSAEAFNOSUPPORT: return SocketError::InvalidArgument;
	case WSAENETUNREACH:
	case WSAEHOSTUNREACH:
	case WSAEHOSTDOWN: return SocketError::NetworkUnreachable;
	case WSAENETDOWN: return SocketError::NetworkDown;
	case WSAEMSGSIZE: return SocketError::MessageTooLarge;
	case WSANOTINITIALISED: return SocketError::NotInitialized;
	default: return SocketError::Unknown;
	}
}


Result<void> ensureInitialized()
{
	static WinsockRuntime runtime;

	if (!runtime.isInitialized())
		return std::unexpected(SocketError::NotInitialized);

	return {};
}


Result<NativeHandle> createSocket()
{
	if (auto init = ensureInitialized(); !init)
		return std::unexpected(init.error());

	const NativeSocket sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	if (sock == INVALID_SOCKET)
		return std::unexpected(lastError());

	if (auto nonBlocking = setNonBlocking(fromNative(sock)); !nonBlocking)
	{
		closesocket(sock);
		return std::unexpected(nonBlocking.error());
	}

	// Without this an ICMP port-unreachable makes the next recvfrom() fail with WSAECONNRESET
	BOOL  reportReset	= FALSE;
	DWORD bytesReturned = 0;
	WSAIoctl(sock, SIO_UDP_CONNRESET, &reportReset, sizeof(reportReset), nullptr, 0, &bytesReturned, nullptr, nullptr);

	return fromNative(sock);
}


Result<void> setNonBlocking(const NativeHandle handle)
{
	u_long nonBlocking = 1;
	if (ioctlsocket(toNative(handle), FIONBIO, &nonBlocking) == SOCKET_ERROR)
		return std::unexpected(lastError());

	return {};
}


void closeHandle(const NativeHandle handle)
{
	if (handle != InvalidNativeHandle)
		closesocket(toNative(handle));
}


void shutdownHandle(const NativeHandle handle)
{
	if (handle != InvalidNativeHandle)
		::shutdown(toNative(handle), SD_BOTH);
}


std::unique_ptr<ReadWaiter> createReadWaiter(const NativeHandle handle)
{
	auto waiter = std::make_unique<ReadWaiterWindows>(toNative(handle));
	return waiter->isValid() ? std::move(waiter) : nullptr;
}

} // namespace netlink::net::platform
