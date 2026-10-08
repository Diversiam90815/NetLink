/*
==============================================================================
	Module:         SocketHandle
	Description:    Move-only RAII owner of a native socket descriptor
  ==============================================================================
*/

#include "SocketHandle.h"
#include "Platform/SocketPlatform.h"

#include <atomic>
#include <utility>


namespace netlink::net
{

struct SocketHandle::State
{
	std::atomic<bool>					  shutdown{false};
	std::unique_ptr<platform::ReadWaiter> waiter;
};


SocketHandle::SocketHandle() = default;


SocketHandle::SocketHandle(const NativeHandle handle) : mHandle(handle), mState(std::make_unique<State>())
{
	mState->waiter = platform::createReadWaiter(handle);
}


SocketHandle::~SocketHandle()
{
	reset();
}


SocketHandle::SocketHandle(SocketHandle &&other) noexcept : mHandle(std::exchange(other.mHandle, InvalidNativeHandle)), mState(std::move(other.mState)) {}


SocketHandle &SocketHandle::operator=(SocketHandle &&other) noexcept
{
	if (this != &other)
	{
		reset();
		mHandle = std::exchange(other.mHandle, InvalidNativeHandle);
		mState	= std::move(other.mState);
	}
	return *this;
}


void SocketHandle::reset()
{
	// The waiter is registered on the descriptor: it has to go first
	mState.reset();

	if (isValid())
		platform::closeHandle(std::exchange(mHandle, InvalidNativeHandle));
}


void SocketHandle::shutdown() const
{
	if (!isValid() || !mState)
		return;

	mState->shutdown.store(true);
	platform::shutdownHandle(mHandle);
	interrupt();
}


bool SocketHandle::isShutdown() const
{
	return mState && mState->shutdown.load();
}


Result<void> SocketHandle::waitReadable(const TimePoint deadline) const
{
	if (!isValid() || !mState || !mState->waiter)
		return std::unexpected(SocketError::InvalidArgument);

	if (isShutdown())
		return std::unexpected(SocketError::Closed);

	auto ready = mState->waiter->wait(deadline);

	if (isShutdown())
		return std::unexpected(SocketError::Closed);

	return ready;
}


void SocketHandle::interrupt() const
{
	if (mState && mState->waiter)
		mState->waiter->interrupt();
}


void SocketHandle::noteReceiveAttempt() const
{
	if (mState && mState->waiter)
		mState->waiter->onReceiveAttempt();
}

} // namespace netlink::net
