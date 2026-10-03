#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "SpyDeadlineTimer.h"
#include "TimeoutService/TimeoutService.h"

using namespace std::chrono_literals;


namespace UtilsTests
{

// ---------------------------------------------------------------------------
// TimeoutKey
// ---------------------------------------------------------------------------

TEST(TimeoutKey, OrderingByCategory)
{
	TimeoutKey a{"alpha", "x"};
	TimeoutKey b{"beta", "x"};
	EXPECT_TRUE(a < b) << "A key with category 'alpha' must sort before one with category 'beta'";
	EXPECT_FALSE(b < a) << "The reverse comparison must be false — ordering must be consistent";
}


TEST(TimeoutKey, OrderingByIdentifierWhenCategoryEqual)
{
	TimeoutKey a{"cat", "aaa"};
	TimeoutKey b{"cat", "bbb"};
	EXPECT_TRUE(a < b) << "When categories are equal, the key with the lexicographically smaller identifier must come first";
	EXPECT_FALSE(b < a) << "The reverse comparison must be false";
}


TEST(TimeoutKey, ToString)
{
	TimeoutKey k{"handshake", "PC-02"};
	EXPECT_EQ(k.toString(), "handshake: PC-02") << "toString() must produce 'category: identifier' with a colon-space separator";
}


// ---------------------------------------------------------------------------
// TimeoutService — state queries
// ---------------------------------------------------------------------------

TEST(TimeoutService, InitiallyEmpty)
{
	TimeoutService svc;
	EXPECT_EQ(svc.activeCount(), 0u) << "A newly constructed TimeoutService must have no active timeouts";
}


TEST(TimeoutService, StartedTimeoutIsActive)
{
	TimeoutService svc;
	TimeoutKey	   key{"cat", "id"};
	svc.startTimeout(key, 500, [](const TimeoutKey &) {});
	EXPECT_TRUE(svc.isActive(key)) << "A timeout must be reported as active immediately after it is started";
	EXPECT_EQ(svc.activeCount(), 1u) << "activeCount() must reflect the one running timeout";
	svc.cancelAll();
}


TEST(TimeoutService, CancelledTimeoutIsNotActive)
{
	TimeoutService svc;
	TimeoutKey	   key{"cat", "id"};
	svc.startTimeout(key, 500, [](const TimeoutKey &) {});
	EXPECT_TRUE(svc.cancelTimeout(key)) << "cancelTimeout() must return true when the key exists and is successfully cancelled";
	EXPECT_FALSE(svc.isActive(key)) << "After cancellation, isActive() must return false for that key";
	EXPECT_EQ(svc.activeCount(), 0u) << "activeCount() must drop to zero after the only timeout is cancelled";
}


TEST(TimeoutService, CancelNonExistentReturnsFalse)
{
	TimeoutService svc;
	EXPECT_FALSE(svc.cancelTimeout({"nonexistent", "none"})) << "cancelTimeout() must return false when the key is not found";
}


// ---------------------------------------------------------------------------
// TimeoutService — callback behaviour
// ---------------------------------------------------------------------------

TEST(TimeoutService, CallbackFiredAfterTimeout)
{
	TimeoutService	  svc;
	std::atomic<bool> fired{false};
	svc.startTimeout({"cat", "id"}, 100, [&](const TimeoutKey &) { fired.store(true); });
	// TimeoutService polls on a 50ms granularity via a real OS thread (std::async), so
	// margin here also has to absorb thread-creation/scheduling jitter
	std::this_thread::sleep_for(500ms);
	EXPECT_TRUE(fired.load()) << "The timeout callback must be invoked after the 100 ms deadline expires";
}


TEST(TimeoutService, CancelledDoesNotFireCallback)
{
	TimeoutService	  svc;
	std::atomic<bool> fired{false};
	TimeoutKey		  key{"cat", "id"};
	svc.startTimeout(key, 100, [&](const TimeoutKey &) { fired.store(true); });
	svc.cancelTimeout(key);
	std::this_thread::sleep_for(200ms);
	EXPECT_FALSE(fired.load()) << "A cancelled timeout must never invoke its callback even after the original deadline";
}


TEST(TimeoutService, RestartTimeoutCancelsPrevious)
{
	TimeoutService	 svc;
	std::atomic<int> count{0};
	TimeoutKey		 key{"cat", "id"};
	svc.startTimeout(key, 100, [&](const TimeoutKey &) { ++count; });
	svc.startTimeout(key, 100, [&](const TimeoutKey &) { ++count; });
	std::this_thread::sleep_for(500ms);
	EXPECT_EQ(count.load(), 1) << "Starting a timeout with an already-active key must cancel the previous one, so the callback fires exactly once";
}


TEST(TimeoutService, MultipleTimeoutsFireIndependently)
{
	TimeoutService	 svc;
	std::atomic<int> count{0};
	svc.startTimeout({"a", "1"}, 100, [&](const TimeoutKey &) { ++count; });
	svc.startTimeout({"b", "2"}, 100, [&](const TimeoutKey &) { ++count; });
	std::this_thread::sleep_for(500ms);
	EXPECT_EQ(count.load(), 2) << "Two independent timeouts must both fire, each incrementing the counter once";
}


// ---------------------------------------------------------------------------
// TimeoutService — bulk cancel
// ---------------------------------------------------------------------------

TEST(TimeoutService, CancelCategoryRemovesAll)
{
	TimeoutService svc;
	svc.startTimeout({"request", "peer-a"}, 500, [](const TimeoutKey &) {});
	svc.startTimeout({"request", "peer-b"}, 500, [](const TimeoutKey &) {});
	EXPECT_EQ(svc.cancelCategory("request"), 2) << "cancelCategory('request') must cancel exactly the 2 timeouts in that category";
	EXPECT_EQ(svc.activeCount(), 0u) << "No timeouts must remain active after all entries in the category are cancelled";
}


TEST(TimeoutService, CancelCategoryLeavesOtherCategories)
{
	TimeoutService svc;
	svc.startTimeout({"request", "peer-a"}, 500, [](const TimeoutKey &) {});
	svc.startTimeout({"handshake", "peer-a"}, 500, [](const TimeoutKey &) {});
	EXPECT_EQ(svc.cancelCategory("request"), 1) << "cancelCategory('request') must cancel only the 1 timeout in that category";
	EXPECT_TRUE(svc.isActive({"handshake", "peer-a"})) << "The timeout in the 'handshake' category must remain active after cancelling 'request'";
	svc.cancelAll();
}


TEST(TimeoutService, CancelByIdentifierRemovesAll)
{
	TimeoutService svc;
	svc.startTimeout({"cat-a", "peer-x"}, 500, [](const TimeoutKey &) {});
	svc.startTimeout({"cat-b", "peer-x"}, 500, [](const TimeoutKey &) {});
	svc.startTimeout({"cat-a", "peer-y"}, 500, [](const TimeoutKey &) {});
	EXPECT_EQ(svc.cancelByIdentifier("peer-x"), 2) << "cancelByIdentifier('peer-x') must cancel the 2 timeouts whose identifier matches";
	EXPECT_FALSE(svc.isActive({"cat-a", "peer-x"})) << "{cat-a, peer-x} must be inactive after cancelByIdentifier('peer-x')";
	EXPECT_FALSE(svc.isActive({"cat-b", "peer-x"})) << "{cat-b, peer-x} must be inactive after cancelByIdentifier('peer-x')";
	EXPECT_TRUE(svc.isActive({"cat-a", "peer-y"})) << "{cat-a, peer-y} must remain active — it has a different identifier";
	svc.cancelAll();
}


TEST(TimeoutService, CancelAllClearsEverything)
{
	TimeoutService svc;
	svc.startTimeout({"a", "1"}, 500, [](const TimeoutKey &) {});
	svc.startTimeout({"b", "2"}, 500, [](const TimeoutKey &) {});
	svc.startTimeout({"c", "3"}, 500, [](const TimeoutKey &) {});
	svc.cancelAll();
	EXPECT_EQ(svc.activeCount(), 0u) << "cancelAll() must leave no active timeouts regardless of how many were running";
}


// ---------------------------------------------------------------------------
// TimeoutService — destructor
// ---------------------------------------------------------------------------

TEST(TimeoutService, DestructorCancelsAll)
{
	std::atomic<bool> fired{false};
	{
		TimeoutService svc;
		svc.startTimeout({"cat", "id"}, 300, [&](const TimeoutKey &) { fired.store(true); });
		// svc is destroyed here — destructor must call cancelAll()
	}
	std::this_thread::sleep_for(50ms);
	EXPECT_FALSE(fired.load()) << "The destructor must cancel all pending timeouts so no callbacks fire after the service is destroyed";
}


// ---------------------------------------------------------------------------
// TimeoutService — deadline order
// ---------------------------------------------------------------------------

TEST(TimeoutService, TimeoutsFireInDeadlineOrder)
{
	TimeoutService			 svc;
	std::mutex				 mutex;
	std::vector<std::string> fired;
	auto					 record = [&](const TimeoutKey &key)
	{
		std::lock_guard<std::mutex> lock(mutex);
		fired.push_back(key.identifier);
	};

	svc.startTimeout({"cat", "late"}, 150, record);
	svc.startTimeout({"cat", "early"}, 50, record);
	svc.startTimeout({"cat", "middle"}, 100, record);
	std::this_thread::sleep_for(500ms);

	std::lock_guard<std::mutex> lock(mutex);
	EXPECT_EQ(fired, (std::vector<std::string>{"early", "middle", "late"})) << "Timeouts started out of order must fire sorted by their deadline";
}


TEST(TimeoutService, RestartWithShorterTimeout_FiresEarlier)
{
	TimeoutService	  svc;
	std::atomic<bool> fired{false};
	TimeoutKey		  key{"cat", "id"};
	svc.startTimeout(key, 3'600'000, [&](const TimeoutKey &) { fired.store(true); });
	svc.startTimeout(key, 10, [&](const TimeoutKey &) { fired.store(true); });
	std::this_thread::sleep_for(300ms);
	EXPECT_TRUE(fired.load()) << "Restarting a key must move its deadline, also to an earlier one";
}


TEST(TimeoutService, CancellingTheEarliest_LaterOnesStillFire)
{
	TimeoutService	  svc;
	std::atomic<bool> earlyFired{false};
	std::atomic<bool> lateFired{false};
	svc.startTimeout({"cat", "early"}, 20, [&](const TimeoutKey &) { earlyFired.store(true); });
	svc.startTimeout({"cat", "late"}, 60, [&](const TimeoutKey &) { lateFired.store(true); });
	svc.cancelTimeout({"cat", "early"});
	std::this_thread::sleep_for(300ms);
	EXPECT_FALSE(earlyFired.load()) << "The cancelled timeout must not fire";
	EXPECT_TRUE(lateFired.load()) << "The timeout behind it must still fire";
}


// ---------------------------------------------------------------------------
// Timer
// ---------------------------------------------------------------------------

TEST(TimeoutService, WaitsOnTheInjectedTimerUntilTheEarliestDeadline)
{
	using Clock				 = netlink::IDeadlineTimer::Clock;

	const auto		  usage	 = std::make_shared<FakeTiming::TimerUsage>();
	TimeoutService	  svc(std::make_unique<FakeTiming::SpyDeadlineTimer>(usage));
	std::atomic<bool> fired{false};

	const auto		  before = Clock::now();
	svc.startTimeout({"cat", "late"}, 3'600'000, [](const TimeoutKey &) {});
	svc.startTimeout({"cat", "early"}, 40, [&](const TimeoutKey &) { fired.store(true); });
	const auto after = Clock::now();

	std::this_thread::sleep_for(300ms);
	ASSERT_TRUE(fired.load());

	EXPECT_GE(usage->wakeCount(), 2) << "Every new timeout must wake the worker, so it re-evaluates what to wait for";

	const auto deadlines = usage->deadlines();
	const bool waitedForEarly =
		std::ranges::any_of(deadlines, [&](const Clock::time_point deadline) { return deadline >= before + 40ms && deadline <= after + 40ms; });
	EXPECT_TRUE(waitedForEarly) << "The worker must wait exactly until the earliest timeout is due";
}


TEST(TimeoutService, ShortTimeout_FiresCloseToItsDeadline)
{
	using Clock = std::chrono::steady_clock;

	TimeoutService svc;

	// The best of several attempts: a busy machine may delay single ones, a coarse timer delays all of them
	auto		   smallestDelay = Clock::duration::max();

	for (int attempt = 0; attempt < 20; ++attempt)
	{
		std::promise<Clock::time_point> firedAt;
		const auto						deadline = Clock::now() + 1ms;

		svc.startTimeout({"cat", "short"}, 1, [&](const TimeoutKey &) { firedAt.set_value(Clock::now()); });

		auto fired = firedAt.get_future();
		ASSERT_EQ(fired.wait_for(2s), std::future_status::ready);
		smallestDelay = std::min(smallestDelay, fired.get() - deadline);
	}

	EXPECT_LT(smallestDelay, 5ms) << "A 1 ms timeout must not be rounded up to the scheduler tick (15.6 ms on Windows)";
}

} // namespace UtilsTests
