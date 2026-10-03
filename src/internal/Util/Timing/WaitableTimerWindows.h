/*
  ==============================================================================
	Module:         WaitableTimerWindows
	Description:    High resolution waitable timer handle (Windows only), shared
					by the deadline timer and the socket waiter
  ==============================================================================
*/

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <timeapi.h> // not part of WIN32_LEAN_AND_MEAN

#include <algorithm>
#include <chrono>
#include <optional>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif


namespace netlink::timing
{

// Owns a waitable timer that fires close to the requested time instead of on the next 15.6 ms scheduler tick.
// Before Windows 10 1803 there is no such timer: the scheduler tick is raised to 1 ms for the lifetime of this object instead.
class WaitableTimer
{
public:
	WaitableTimer()
	{
		mHandle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

		if (!mHandle)
		{
			mHandle			= CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
			mRaisedTickRate = timeBeginPeriod(1) == TIMERR_NOERROR;
		}
	}

	~WaitableTimer()
	{
		if (mHandle)
			CloseHandle(mHandle);

		if (mRaisedTickRate)
			timeEndPeriod(1);
	}

	WaitableTimer(const WaitableTimer &)			= delete;
	WaitableTimer &operator=(const WaitableTimer &) = delete;

	using Clock										= std::chrono::steady_clock;
	using TimePoint									= Clock::time_point;

	HANDLE handle() const { return mHandle; }
	bool   isValid() const { return mHandle != nullptr; }

	// Makes sure the handle is signalled no later than the deadline
	void   armFor(const TimePoint deadline, const TimePoint now)
	{
		if (mArmedFor && *mArmedFor <= deadline && *mArmedFor > now)
			return;

		LARGE_INTEGER due{};
		due.QuadPart = -std::max<LONGLONG>(std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count() / 100, 1); // negative: relative, in 100 ns units
		SetWaitableTimer(mHandle, &due, 0, nullptr, nullptr, FALSE);
		mArmedFor = deadline;
	}

	// A wait ended on the timer handle: it is not running anymore
	void onSignalled() { mArmedFor.reset(); }

private:
	HANDLE					 mHandle{nullptr};
	bool					 mRaisedTickRate{false};
	std::optional<TimePoint> mArmedFor; // only touched by the waiting thread
};

} // namespace netlink::timing
