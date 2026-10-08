/*
==============================================================================
	Module:         SocketPlatform
	Description:    Thin compile-time abstraction over the native socket API.
  ==============================================================================
*/

#pragma once

#include <chrono>
#include <memory>
#include <span>

#include "Socket/SocketHandle.h"
#include "Socket/SocketTypes.h"


namespace netlink::net::platform
{

// Waits on one socket until it is readable, a deadline passed or another thread interrupted the wait.
//	Windows		socket event + interrupt event + high resolution timer
//	Linux		ppoll() with an eventfd
//	macOS		kqueue
class ReadWaiter
{
public:
	virtual ~ReadWaiter()																  = default;

	// Fails with SocketError::Timeout once the deadline passed and SocketError::Cancelled after interrupt()
	virtual Result<void> wait(std::chrono::steady_clock::time_point deadline) = 0;

	// Ends the wait in progress, or the next one if none is in progress
	virtual void		 interrupt()										  = 0;

	// The owner of the socket tried to read from it. Only of interest where the operating system reports readability
	// once per arrival instead of for as long as data is waiting (Windows).
	virtual void		 onReceiveAttempt() {}
};


// --- OS specific ---------------


// Initializes the socket subsystem once per process
Result<void>		  ensureInitialized();

// Creates a non-blocking UDP socket
Result<NativeHandle>  createSocket();

Result<void>		  setNonBlocking(NativeHandle handle);

void				  closeHandle(NativeHandle handle);
void				  shutdownHandle(NativeHandle handle);

// Null if the operating system refused the resources of a waiter. The waiter must be destroyed before the handle is closed.
std::unique_ptr<ReadWaiter> createReadWaiter(NativeHandle handle);


// --- Socket Common Handles ------------------------------------------------

// Last native error of the calling thread, mapped to a portable error.
SocketError			  lastError();

Result<void>		  applyBindOptions(NativeHandle handle, const BindOptions &options);
Result<void>		  bindTo(NativeHandle handle, const SocketAddress &address);

Result<SocketAddress> localAddressOf(NativeHandle handle);

Result<size_t>		  sendDatagram(NativeHandle handle, const SocketAddress &to, std::span<const uint8_t> data);

// Sends head and body as one datagram without joining them first
Result<size_t>		  sendDatagram(NativeHandle handle, const SocketAddress &to, std::span<const uint8_t> head, std::span<const uint8_t> body);
Result<Datagram>	  receiveDatagram(NativeHandle handle, std::span<uint8_t> buffer);

} // namespace netlink::net::platform
