/*
==============================================================================
	Module:         SocketPlatformWindows
	Description:    Winsock specific part of the native socket abstraction
  ==============================================================================
*/

#include "SocketPlatform.h"
#include "SocketCommon.h"

#include <algorithm>

#include <mstcpip.h>

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

} // namespace


SocketError common::mapNativeError(int code)
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
	case WSAENETDOWN: return SocketError::NetworkUnreachable;
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


Result<NativeHandle> createSocket(SocketKind kind)
{
	if (auto init = ensureInitialized(); !init)
		return std::unexpected(init.error());

	const bool	 isStream = kind == SocketKind::Stream;
	NativeSocket sock	  = ::socket(AF_INET, isStream ? SOCK_STREAM : SOCK_DGRAM, isStream ? IPPROTO_TCP : IPPROTO_UDP);

	if (sock == INVALID_SOCKET)
		return std::unexpected(lastError());

	if (auto nonBlocking = setNonBlocking(fromNative(sock)); !nonBlocking)
	{
		closesocket(sock);
		return std::unexpected(nonBlocking.error());
	}

	if (!isStream)
	{
		BOOL  reportReset	= FALSE;
		DWORD bytesReturned = 0;
		WSAIoctl(sock, SIO_UDP_CONNRESET, &reportReset, sizeof(reportReset), nullptr, 0, &bytesReturned, nullptr, nullptr);
	}

	return fromNative(sock);
}


Result<void> setNonBlocking(NativeHandle handle)
{
	u_long nonBlocking = 1;
	if (ioctlsocket(toNative(handle), FIONBIO, &nonBlocking) == SOCKET_ERROR)
		return std::unexpected(lastError());

	return {};
}


Result<void> prepareAcceptedSocket(NativeHandle handle)
{
	return setNonBlocking(handle);
}


void closeHandle(NativeHandle handle)
{
	if (handle != InvalidNativeHandle)
		closesocket(toNative(handle));
}


void shutdownHandle(NativeHandle handle)
{
	if (handle != InvalidNativeHandle)
		::shutdown(toNative(handle), SD_BOTH);
}


Result<void> waitUntil(NativeHandle handle, WaitFor what, std::chrono::milliseconds timeout)
{
	if (handle == InvalidNativeHandle)
		return std::unexpected(SocketError::InvalidArgument);

	fd_set primary{};
	fd_set exceptions{};
	FD_ZERO(&primary);
	FD_ZERO(&exceptions);
	FD_SET(toNative(handle), &primary);
	FD_SET(toNative(handle), &exceptions);

	const auto clamped = std::max<std::chrono::milliseconds::rep>(timeout.count(), 0);

	timeval	   tv{};
	tv.tv_sec		   = static_cast<long>(clamped / 1000);
	tv.tv_usec		   = static_cast<long>((clamped % 1000) * 1000);

	const bool reading = what == WaitFor::Readable;
	const int  result  = ::select(0, reading ? &primary : nullptr, reading ? nullptr : &primary, &exceptions, &tv);

	if (result == SOCKET_ERROR)
		return std::unexpected(lastError());

	if (result == 0)
		return std::unexpected(SocketError::Timeout);

	return {}; // ready, or an exceptional condition the following call will report
}

} // namespace netlink::net::platform
