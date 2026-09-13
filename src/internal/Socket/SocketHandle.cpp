/*
==============================================================================
	Module:         SocketHandle
	Description:    Move-only RAII owner of a native socket descriptor
  ==============================================================================
*/

#include "SocketHandle.h"
#include "Platform/SocketPlatform.h"

#include <algorithm>
#include <utility>


namespace netlink::net
{

namespace
{
// Upper bound for noticing a shutdown() from another thread
constexpr std::chrono::milliseconds ShutdownPollSlice{50};
} // namespace


void SocketHandle::reset()
{
	if (isValid())
		platform::closeHandle(std::exchange(mHandle, InvalidNativeHandle));
}


void SocketHandle::shutdown() const
{
	if (!isValid())
		return;

	mShutdown->store(true);
	platform::shutdownHandle(mHandle);
}


Result<void> SocketHandle::wait(WaitFor what, std::chrono::milliseconds timeout) const
{
	using Clock			= std::chrono::steady_clock;
	const auto deadline = Clock::now() + timeout;

	while (true)
	{
		if (isShutdown())
			return std::unexpected(SocketError::Closed);

		const auto remaining = std::max(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()), std::chrono::milliseconds{0});

		auto	   ready	 = platform::waitUntil(mHandle, what, std::min(remaining, ShutdownPollSlice));

		if (ready || ready.error() != SocketError::Timeout)
			return isShutdown() ? Result<void>(std::unexpected(SocketError::Closed)) : ready;

		if (Clock::now() >= deadline)
			return std::unexpected(SocketError::Timeout);
	}
}

} // namespace netlink::net
