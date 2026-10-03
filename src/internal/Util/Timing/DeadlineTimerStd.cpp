/*
  ==============================================================================
	Module:         DeadlineTimerStd
	Description:    Deadline timer on a condition variable (Linux, macOS)
  ==============================================================================
*/

#include "DeadlineTimer.h"

#include <condition_variable>
#include <mutex>
#include <utility>


namespace netlink
{

namespace
{

class DeadlineTimerStd final : public IDeadlineTimer
{
public:
	WaitResult waitUntil(const TimePoint deadline) override
	{
		std::unique_lock<std::mutex> lock(mMutex);

		if (deadline == Never)
			mChanged.wait(lock, [this] { return mWoken; });
		else
			mChanged.wait_until(lock, deadline, [this] { return mWoken; });

		return std::exchange(mWoken, false) ? WaitResult::Woken : WaitResult::Expired;
	}

	void wake() override
	{
		{
			std::lock_guard<std::mutex> lock(mMutex);
			mWoken = true;
		}
		mChanged.notify_one();
	}

private:
	std::mutex				mMutex;
	std::condition_variable mChanged;
	bool					mWoken{false};
};

} // namespace


std::unique_ptr<IDeadlineTimer> makeDeadlineTimer()
{
	return std::make_unique<DeadlineTimerStd>();
}

} // namespace netlink
