/*
==============================================================================
	Module:         SocketHandle
	Description:    Move-only RAII owner of a native socket descriptor
  ==============================================================================
*/

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>

#include "SocketTypes.h"


namespace netlink::net
{

// Wide enough for a Winsock SOCKET (UINT_PTR) & a POSIX file descriptor.
using NativeHandle								  = uintptr_t;
inline constexpr NativeHandle InvalidNativeHandle = static_cast<NativeHandle>(~static_cast<NativeHandle>(0));


enum class WaitFor : uint8_t
{
	Readable,
	Writable,
};


class SocketHandle
{
public:
	SocketHandle() = default;
	explicit SocketHandle(NativeHandle handle) : mHandle(handle), mShutdown(std::make_unique<std::atomic<bool>>(false)) {}
	~SocketHandle() { reset(); }

	SocketHandle(const SocketHandle &)			  = delete;
	SocketHandle &operator=(const SocketHandle &) = delete;

	SocketHandle(SocketHandle &&other) noexcept : mHandle(std::exchange(other.mHandle, InvalidNativeHandle)), mShutdown(std::move(other.mShutdown)) {}
	SocketHandle &operator=(SocketHandle &&other) noexcept
	{
		if (this != &other)
		{
			reset();
			mHandle	  = std::exchange(other.mHandle, InvalidNativeHandle);
			mShutdown = std::move(other.mShutdown);
		}
		return *this;
	}

	NativeHandle get() const { return mHandle; }
	bool		 isValid() const { return mHandle != InvalidNativeHandle; }

	void		 reset();

	void		 shutdown() const;
	bool		 isShutdown() const { return mShutdown && mShutdown->load(); }

	Result<void> wait(WaitFor what, std::chrono::milliseconds timeout) const;

private:
	NativeHandle					   mHandle = InvalidNativeHandle;
	std::unique_ptr<std::atomic<bool>> mShutdown; // keep a stable address across moves
};

} // namespace netlink::net
