/*
==============================================================================
	Module:         SocketHandle
	Description:    Move-only RAII owner of a native socket descriptor
  ==============================================================================
*/

#pragma once

#include <chrono>
#include <memory>

#include "SocketTypes.h"


namespace netlink::net
{

// Wide enough for a Winsock SOCKET (UINT_PTR) & a POSIX file descriptor.
using NativeHandle								  = uintptr_t;
inline constexpr NativeHandle InvalidNativeHandle = static_cast<NativeHandle>(~static_cast<NativeHandle>(0));


class SocketHandle
{
public:
	using Clock		= std::chrono::steady_clock;
	using TimePoint = Clock::time_point;

	SocketHandle();
	explicit SocketHandle(NativeHandle handle);
	~SocketHandle();

	SocketHandle(const SocketHandle &)			  = delete;
	SocketHandle &operator=(const SocketHandle &) = delete;

	SocketHandle(SocketHandle &&other) noexcept;
	SocketHandle &operator=(SocketHandle &&other) noexcept;

	NativeHandle  get() const { return mHandle; }
	bool		  isValid() const { return mHandle != InvalidNativeHandle; }

	void		  reset();

	void		  shutdown() const;
	bool		  isShutdown() const;

	// Waits until a datagram can be read. Fails with Timeout once the deadline passed, Cancelled after interrupt()
	// and Closed after shutdown().
	Result<void>  waitReadable(TimePoint deadline) const;

	// Ends the waitReadable() in progress on another thread, or the next one if none is in progress
	void		  interrupt() const;

	// To be called with every read from the descriptor, so waitReadable() knows what was consumed
	void		  noteReceiveAttempt() const;

private:
	struct State; // shared with waiting threads: keeps a stable address across moves

	NativeHandle		   mHandle = InvalidNativeHandle;
	std::unique_ptr<State> mState;
};

} // namespace netlink::net
