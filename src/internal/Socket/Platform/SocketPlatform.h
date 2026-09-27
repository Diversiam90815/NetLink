/*
==============================================================================
	Module:         SocketPlatform
	Description:    Thin compile-time abstraction over the native socket API.
  ==============================================================================
*/

#pragma once

#include <chrono>
#include <span>

#include "Socket/SocketHandle.h"
#include "Socket/SocketTypes.h"


namespace netlink::net::platform
{

// --- OS specific ---------------


// Initializes the socket subsystem once per process
Result<void>		  ensureInitialized();

// Creates a non-blocking UDP socket
Result<NativeHandle>  createSocket();

Result<void>		  setNonBlocking(NativeHandle handle);

void				  closeHandle(NativeHandle handle);
void				  shutdownHandle(NativeHandle handle);

// Waits until the socket is ready. Returns SocketError::Timeout when the wait expired.
Result<void>		  waitUntil(NativeHandle handle, WaitFor what, std::chrono::milliseconds timeout);


// --- Socket Common Handles ------------------------------------------------

// Last native error of the calling thread, mapped to a portable error.
SocketError			  lastError();

Result<void>		  applyBindOptions(NativeHandle handle, const BindOptions &options);
Result<void>		  bindTo(NativeHandle handle, const SocketAddress &address);

Result<SocketAddress> localAddressOf(NativeHandle handle);

Result<size_t>		  sendDatagram(NativeHandle handle, const SocketAddress &to, std::span<const uint8_t> data);
Result<Datagram>	  receiveDatagram(NativeHandle handle, std::span<uint8_t> buffer);

} // namespace netlink::net::platform
