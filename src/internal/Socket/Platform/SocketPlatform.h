/*
==============================================================================
	Module:         SocketPlatform
	Description:    Thin compile-time abstraction over the native socket API.
  ==============================================================================
*/

#pragma once

#include <chrono>
#include <span>
#include <utility>

#include "Socket/SocketHandle.h"
#include "Socket/SocketTypes.h"


namespace netlink::net::platform
{

enum class SocketKind : uint8_t
{
	Datagram,
	Stream,
};


// --- OS specific ---------------


// Initializes the socket subsystem once per process
Result<void>								   ensureInitialized();

// Creates a non-blocking socket
Result<NativeHandle>						   createSocket(SocketKind kind);

Result<void>								   setNonBlocking(NativeHandle handle);

// Applies the createSocket() invariants to a socket returned by accept()
Result<void>								   prepareAcceptedSocket(NativeHandle handle);

void										   closeHandle(NativeHandle handle);
void										   shutdownHandle(NativeHandle handle);

// Waits until the socket is ready. Returns SocketError::Timeout when the wait expired.
Result<void>								   waitUntil(NativeHandle handle, WaitFor what, std::chrono::milliseconds timeout);


// --- Socket Common Handles ------------------------------------------------

// Last native error of the calling thread, mapped to a portable error.
SocketError									   lastError();

Result<void>								   applyBindOptions(NativeHandle handle, const BindOptions &options);
Result<void>								   setNoDelay(NativeHandle handle);
Result<void>								   bindTo(NativeHandle handle, const SocketAddress &address);
Result<void>								   listenOn(NativeHandle handle, int backlog);
Result<std::pair<NativeHandle, SocketAddress>> acceptOne(NativeHandle listener);

// Starts a non-blocking connect. Succeeds immediately or fails with WouldBlock while in progress.
Result<void>								   startConnect(NativeHandle handle, const SocketAddress &remote);

// Outcome of a connect that was in progress once the socket became writable.
Result<void>								   finishConnect(NativeHandle handle);

Result<SocketAddress>						   localAddressOf(NativeHandle handle);
Result<SocketAddress>						   remoteAddressOf(NativeHandle handle);

Result<size_t>								   sendSome(NativeHandle handle, std::span<const uint8_t> data);
Result<size_t>								   receiveSome(NativeHandle handle, std::span<uint8_t> buffer); // 0 bytes -> SocketError::Closed
Result<size_t>								   sendDatagram(NativeHandle handle, const SocketAddress &to, std::span<const uint8_t> data);
Result<Datagram>							   receiveDatagram(NativeHandle handle, std::span<uint8_t> buffer);

} // namespace netlink::net::platform
