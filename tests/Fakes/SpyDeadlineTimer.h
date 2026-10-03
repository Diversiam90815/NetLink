/*
  ==============================================================================
	Module:         SpyDeadlineTimer
	Description:    Records how a deadline timer is used, while the platform's
					timer does the waiting
  ==============================================================================
*/

#pragma once

#include <memory>
#include <mutex>
#include <vector>

#include "Util/Timing/DeadlineTimer.h"


namespace FakeTiming
{

// Shared with the test: the timer itself is owned by the object under test
struct TimerUsage
{
	std::vector<netlink::IDeadlineTimer::TimePoint> deadlines() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return waitedFor;
	}

	int wakeCount() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return wakes;
	}

	mutable std::mutex								mutex;
	std::vector<netlink::IDeadlineTimer::TimePoint> waitedFor; // the deadline of every wait, in order
	int												wakes{0};
};


class SpyDeadlineTimer final : public netlink::IDeadlineTimer
{
public:
	explicit SpyDeadlineTimer(std::shared_ptr<TimerUsage> usage) : mUsage(std::move(usage)) {}

	netlink::WaitResult waitUntil(const TimePoint deadline) override
	{
		{
			std::lock_guard<std::mutex> lock(mUsage->mutex);
			mUsage->waitedFor.push_back(deadline);
		}
		return mInner->waitUntil(deadline);
	}

	void wake() override
	{
		{
			std::lock_guard<std::mutex> lock(mUsage->mutex);
			++mUsage->wakes;
		}
		mInner->wake();
	}

private:
	std::shared_ptr<TimerUsage>				 mUsage;
	std::unique_ptr<netlink::IDeadlineTimer> mInner{netlink::makeDeadlineTimer()};
};

} // namespace FakeTiming
