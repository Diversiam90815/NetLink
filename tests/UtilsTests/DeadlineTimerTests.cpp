#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <thread>

#include "Util/Timing/DeadlineTimer.h"

using namespace netlink;
using namespace std::chrono_literals;


namespace UtilsTests
{

using Clock = IDeadlineTimer::Clock;


TEST(DeadlineTimer, ExpiresWhenTheDeadlinePasses)
{
	const auto timer   = makeDeadlineTimer();
	const auto started = Clock::now();

	EXPECT_EQ(timer->waitUntil(started + 30ms), WaitResult::Expired);
	EXPECT_GE(Clock::now() - started, 30ms) << "The wait must not end before the deadline";
}


TEST(DeadlineTimer, DeadlineInThePast_ExpiresImmediately)
{
	const auto timer   = makeDeadlineTimer();
	const auto started = Clock::now();

	EXPECT_EQ(timer->waitUntil(started - 1s), WaitResult::Expired);
	EXPECT_LT(Clock::now() - started, 500ms);
}


TEST(DeadlineTimer, Wake_EndsTheWait)
{
	const auto timer   = makeDeadlineTimer();
	const auto started = Clock::now();

	auto	   waiting = std::async(std::launch::async, [&] { return timer->waitUntil(started + 10s); });
	std::this_thread::sleep_for(30ms);
	timer->wake();

	ASSERT_EQ(waiting.wait_for(2s), std::future_status::ready) << "wake() must end the wait long before its deadline";
	EXPECT_EQ(waiting.get(), WaitResult::Woken);
}


TEST(DeadlineTimer, WakeBeforeTheWait_IsNotLost)
{
	const auto timer = makeDeadlineTimer();
	timer->wake();

	const auto started = Clock::now();
	EXPECT_EQ(timer->waitUntil(started + 10s), WaitResult::Woken) << "A wake() without a wait in progress must end the next wait";
	EXPECT_LT(Clock::now() - started, 2s);

	EXPECT_EQ(timer->waitUntil(Clock::now() + 20ms), WaitResult::Expired) << "One wake() ends exactly one wait";
}


TEST(DeadlineTimer, WakeBeforeAnExpiredDeadline_StillCounts)
{
	const auto timer = makeDeadlineTimer();
	timer->wake();

	EXPECT_EQ(timer->waitUntil(Clock::now() - 1s), WaitResult::Woken) << "The wake-up must not be swallowed by a deadline that already passed";
}


TEST(DeadlineTimer, Never_WaitsUntilWoken)
{
	const auto timer   = makeDeadlineTimer();

	auto	   waiting = std::async(std::launch::async, [&] { return timer->waitUntil(IDeadlineTimer::Never); });
	EXPECT_EQ(waiting.wait_for(100ms), std::future_status::timeout) << "Without a deadline only wake() may end the wait";

	timer->wake();
	ASSERT_EQ(waiting.wait_for(2s), std::future_status::ready);
	EXPECT_EQ(waiting.get(), WaitResult::Woken);
}


TEST(DeadlineTimer, ShortWaits_EndCloseToTheirDeadline)
{
	const auto timer = makeDeadlineTimer();

	// The best of several attempts: a busy machine may delay single ones, a coarse timer delays all of them
	auto	   smallestDelay = Clock::duration::max();

	for (int attempt = 0; attempt < 20; ++attempt)
	{
		const auto deadline = Clock::now() + 1ms;
		timer->waitUntil(deadline);
		smallestDelay = std::min(smallestDelay, Clock::now() - deadline);
	}

	EXPECT_LT(smallestDelay, 5ms) << "A 1 ms wait must not be rounded up to the scheduler tick (15.6 ms on Windows)";
}

} // namespace UtilsTests
