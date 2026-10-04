/*
  ==============================================================================
	Module:         DeadlineTimerWindows
	Description:    Deadline timer on a high resolution waitable timer
  ==============================================================================
*/

#include "DeadlineTimer.h"
#include "Timing/WaitableTimerWindows.h"


namespace netlink
{

namespace
{

class DeadlineTimerWindows final : public IDeadlineTimer
{
public:
	DeadlineTimerWindows() : mWake(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}

	~DeadlineTimerWindows() override
	{
		if (mWake)
			CloseHandle(mWake);
	}

	WaitResult waitUntil(const TimePoint deadline) override
	{
		if (deadline == Never)
		{
			WaitForSingleObject(mWake, INFINITE);
			return WaitResult::Woken;
		}

		// The wake event comes first: it wins when both are signalled
		const HANDLE handles[] = {mWake, mTimer.handle()};

		while (true)
		{
			const auto now = Clock::now();

			if (now >= deadline)
				return wasWoken() ? WaitResult::Woken : WaitResult::Expired;

			mTimer.armFor(deadline, now);

			if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0)
				return WaitResult::Woken;

			mTimer.onSignalled(); // possibly for an earlier deadline: the clock decides
		}
	}

	void wake() override { SetEvent(mWake); }

private:
	// Consumes a pending wake()
	bool				  wasWoken() const { return WaitForSingleObject(mWake, 0) == WAIT_OBJECT_0; }

	timing::WaitableTimer mTimer;
	HANDLE				  mWake; // auto-reset: one wake() ends one wait
};

} // namespace


std::unique_ptr<IDeadlineTimer> makeDeadlineTimer()
{
	return std::make_unique<DeadlineTimerWindows>();
}

} // namespace netlink
