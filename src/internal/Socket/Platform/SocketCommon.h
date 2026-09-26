/*
==============================================================================
	Module:         SocketCommon
	Description:    Common interfaces for syntactic differences between Winsock and POSIX sockets
  ==============================================================================
*/

#pragma once

#include "Socket/SocketHandle.h"
#include "Socket/SocketTypes.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif


namespace netlink::net::platform::common
{

#if defined(_WIN32)

using NativeSocket					= SOCKET;
using SockLen						= int;
using BufferLength					= int;
inline constexpr int SendFlags		= 0;
inline constexpr int ShutdownBoth	= SD_BOTH;
inline constexpr int SocketErrorRet = SOCKET_ERROR;

inline int			 lastNativeError()
{
	return WSAGetLastError();
}
inline bool isWouldBlock(int code)
{
	return code == WSAEWOULDBLOCK;
}
inline bool isInterrupted(int code)
{
	return code == WSAEINTR;
}

#else

using NativeSocket = int;
using SockLen	   = socklen_t;
using BufferLength = size_t;
#if defined(MSG_NOSIGNAL)
inline constexpr int SendFlags = MSG_NOSIGNAL; // Linux: never raise SIGPIPE on a closed connection
#else
inline constexpr int SendFlags = 0; // macOS: SO_NOSIGPIPE is set on socket creation instead
#endif
inline constexpr int ShutdownBoth	= SHUT_RDWR;
inline constexpr int SocketErrorRet = -1;

inline int			 lastNativeError()
{
	return errno;
}
inline bool isWouldBlock(int code)
{
	return code == EAGAIN || code == EWOULDBLOCK;
}
inline bool isInterrupted(int code)
{
	return code == EINTR;
}

#endif


inline NativeSocket toNative(NativeHandle handle)
{
	return static_cast<NativeSocket>(handle);
}

inline NativeHandle fromNative(NativeSocket socket)
{
	return static_cast<NativeHandle>(socket);
}

// Implemented per platform
SocketError mapNativeError(int code);

} // namespace netlink::net::platform::common
