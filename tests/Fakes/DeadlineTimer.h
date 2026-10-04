/*
  ==============================================================================
	Module:         DeadlineTimer
	Description:    Interruptible wait until a point in time, with one impl per platform
  ==============================================================================
*/

#pragma once

#include <chrono>
#include <memory>


namespace netlink
{

enum class WaitResult
{
	Expired, // the deadline passed
	Woken,	 // wake() was called
};


/*
 One thread waits, any thread may wake it. A wake() is never lost: without a wait in progress, it ends the next one.

 Implementations:
	Windows			high resolution waitable timer (the default timers only fire on the 15.6 ms scheduler tick)
	Linux, macOS	condition variable on the monotonic clock
 */
class IDeadlineTimer
{
public:
	using Clock										 = std::chrono::steady_clock;
	using TimePoint									 = Clock::time_point;

	// Waits without a deadline: only wake() ends it
	static constexpr TimePoint Never				 = TimePoint::max();

	virtual ~IDeadlineTimer()						 = default;

	virtual WaitResult waitUntil(TimePoint deadline) = 0;
	virtual void	   wake()						 = 0;
};


// The timer of the platform this library was built for
std::unique_ptr<IDeadlineTimer> makeDeadlineTimer();

} // namespace netlink
