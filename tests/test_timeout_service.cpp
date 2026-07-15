#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include "TimeoutService/TimeoutService.h"

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// TimeoutKey
// ---------------------------------------------------------------------------

TEST(TimeoutKey, OrderingByCategory)
{
    TimeoutKey a{"alpha", "x"};
    TimeoutKey b{"beta", "x"};
    EXPECT_TRUE(a < b)
        << "A key with category 'alpha' must sort before one with category 'beta'";
    EXPECT_FALSE(b < a)
        << "The reverse comparison must be false — ordering must be consistent";
}

TEST(TimeoutKey, OrderingByIdentifierWhenCategoryEqual)
{
    TimeoutKey a{"cat", "aaa"};
    TimeoutKey b{"cat", "bbb"};
    EXPECT_TRUE(a < b)
        << "When categories are equal, the key with the lexicographically smaller identifier must come first";
    EXPECT_FALSE(b < a)
        << "The reverse comparison must be false";
}

TEST(TimeoutKey, ToString)
{
    TimeoutKey k{"handshake", "PC-02"};
    EXPECT_EQ(k.toString(), "handshake: PC-02")
        << "toString() must produce 'category: identifier' with a colon-space separator";
}

// ---------------------------------------------------------------------------
// TimeoutService — state queries
// ---------------------------------------------------------------------------

TEST(TimeoutService, InitiallyEmpty)
{
    TimeoutService svc;
    EXPECT_EQ(svc.activeCount(), 0u)
        << "A newly constructed TimeoutService must have no active timeouts";
}

TEST(TimeoutService, StartedTimeoutIsActive)
{
    TimeoutService svc;
    TimeoutKey     key{"cat", "id"};
    svc.startTimeout(key, 500, [](const TimeoutKey &) {});
    EXPECT_TRUE(svc.isActive(key))
        << "A timeout must be reported as active immediately after it is started";
    EXPECT_EQ(svc.activeCount(), 1u)
        << "activeCount() must reflect the one running timeout";
    svc.cancelAll();
}

TEST(TimeoutService, CancelledTimeoutIsNotActive)
{
    TimeoutService svc;
    TimeoutKey     key{"cat", "id"};
    svc.startTimeout(key, 500, [](const TimeoutKey &) {});
    EXPECT_TRUE(svc.cancelTimeout(key))
        << "cancelTimeout() must return true when the key exists and is successfully cancelled";
    EXPECT_FALSE(svc.isActive(key))
        << "After cancellation, isActive() must return false for that key";
    EXPECT_EQ(svc.activeCount(), 0u)
        << "activeCount() must drop to zero after the only timeout is cancelled";
}

TEST(TimeoutService, CancelNonExistentReturnsFalse)
{
    TimeoutService svc;
    EXPECT_FALSE(svc.cancelTimeout({"nonexistent", "none"}))
        << "cancelTimeout() must return false when the key is not found";
}

// ---------------------------------------------------------------------------
// TimeoutService — callback behaviour
// ---------------------------------------------------------------------------

TEST(TimeoutService, CallbackFiredAfterTimeout)
{
    TimeoutService    svc;
    std::atomic<bool> fired{false};
    svc.startTimeout({"cat", "id"}, 100, [&](const TimeoutKey &) { fired.store(true); });
    // TimeoutService polls on a 50ms granularity via a real OS thread (std::async), so
    // margin here also has to absorb thread-creation/scheduling jitter - generous on
    // slower/contended CI runners (observed flaky on GitHub's macOS runners at 200ms).
    std::this_thread::sleep_for(500ms);
    EXPECT_TRUE(fired.load())
        << "The timeout callback must be invoked after the 100 ms deadline expires";
}

TEST(TimeoutService, CancelledDoesNotFireCallback)
{
    TimeoutService    svc;
    std::atomic<bool> fired{false};
    TimeoutKey        key{"cat", "id"};
    svc.startTimeout(key, 100, [&](const TimeoutKey &) { fired.store(true); });
    svc.cancelTimeout(key);
    std::this_thread::sleep_for(200ms);
    EXPECT_FALSE(fired.load())
        << "A cancelled timeout must never invoke its callback even after the original deadline";
}

TEST(TimeoutService, RestartTimeoutCancelsPrevious)
{
    TimeoutService   svc;
    std::atomic<int> count{0};
    TimeoutKey       key{"cat", "id"};
    svc.startTimeout(key, 100, [&](const TimeoutKey &) { ++count; });
    svc.startTimeout(key, 100, [&](const TimeoutKey &) { ++count; });
    // Restarting blocks on cancelling the first timer (up to one 50ms poll interval)
    // before the second timer's 100ms starts counting, so give extra slack on top of
    // that for CI scheduling jitter (observed flaky on GitHub's macOS runners at 250ms).
    std::this_thread::sleep_for(500ms);
    EXPECT_EQ(count.load(), 1)
        << "Starting a timeout with an already-active key must cancel the previous one, so the callback fires exactly once";
}

TEST(TimeoutService, MultipleTimeoutsFireIndependently)
{
    TimeoutService   svc;
    std::atomic<int> count{0};
    svc.startTimeout({"a", "1"}, 100, [&](const TimeoutKey &) { ++count; });
    svc.startTimeout({"b", "2"}, 100, [&](const TimeoutKey &) { ++count; });
    // See CallbackFiredAfterTimeout for why the margin is this wide.
    std::this_thread::sleep_for(500ms);
    EXPECT_EQ(count.load(), 2)
        << "Two independent timeouts must both fire, each incrementing the counter once";
}

// ---------------------------------------------------------------------------
// TimeoutService — bulk cancel
// ---------------------------------------------------------------------------

TEST(TimeoutService, CancelCategoryRemovesAll)
{
    TimeoutService svc;
    svc.startTimeout({"request", "peer-a"}, 500, [](const TimeoutKey &) {});
    svc.startTimeout({"request", "peer-b"}, 500, [](const TimeoutKey &) {});
    EXPECT_EQ(svc.cancelCategory("request"), 2)
        << "cancelCategory('request') must cancel exactly the 2 timeouts in that category";
    EXPECT_EQ(svc.activeCount(), 0u)
        << "No timeouts must remain active after all entries in the category are cancelled";
}

TEST(TimeoutService, CancelCategoryLeavesOtherCategories)
{
    TimeoutService svc;
    svc.startTimeout({"request", "peer-a"}, 500, [](const TimeoutKey &) {});
    svc.startTimeout({"handshake", "peer-a"}, 500, [](const TimeoutKey &) {});
    EXPECT_EQ(svc.cancelCategory("request"), 1)
        << "cancelCategory('request') must cancel only the 1 timeout in that category";
    EXPECT_TRUE(svc.isActive({"handshake", "peer-a"}))
        << "The timeout in the 'handshake' category must remain active after cancelling 'request'";
    svc.cancelAll();
}

TEST(TimeoutService, CancelByIdentifierRemovesAll)
{
    TimeoutService svc;
    svc.startTimeout({"cat-a", "peer-x"}, 500, [](const TimeoutKey &) {});
    svc.startTimeout({"cat-b", "peer-x"}, 500, [](const TimeoutKey &) {});
    svc.startTimeout({"cat-a", "peer-y"}, 500, [](const TimeoutKey &) {});
    EXPECT_EQ(svc.cancelByIdentifier("peer-x"), 2)
        << "cancelByIdentifier('peer-x') must cancel the 2 timeouts whose identifier matches";
    EXPECT_FALSE(svc.isActive({"cat-a", "peer-x"}))
        << "{cat-a, peer-x} must be inactive after cancelByIdentifier('peer-x')";
    EXPECT_FALSE(svc.isActive({"cat-b", "peer-x"}))
        << "{cat-b, peer-x} must be inactive after cancelByIdentifier('peer-x')";
    EXPECT_TRUE(svc.isActive({"cat-a", "peer-y"}))
        << "{cat-a, peer-y} must remain active — it has a different identifier";
    svc.cancelAll();
}

TEST(TimeoutService, CancelAllClearsEverything)
{
    TimeoutService svc;
    svc.startTimeout({"a", "1"}, 500, [](const TimeoutKey &) {});
    svc.startTimeout({"b", "2"}, 500, [](const TimeoutKey &) {});
    svc.startTimeout({"c", "3"}, 500, [](const TimeoutKey &) {});
    svc.cancelAll();
    EXPECT_EQ(svc.activeCount(), 0u)
        << "cancelAll() must leave no active timeouts regardless of how many were running";
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
    EXPECT_FALSE(fired.load())
        << "The destructor must cancel all pending timeouts so no callbacks fire after the service is destroyed";
}
