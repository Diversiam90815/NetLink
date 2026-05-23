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
    EXPECT_TRUE(a < b);
    EXPECT_FALSE(b < a);
}

TEST(TimeoutKey, OrderingByIdentifierWhenCategoryEqual)
{
    TimeoutKey a{"cat", "aaa"};
    TimeoutKey b{"cat", "bbb"};
    EXPECT_TRUE(a < b);
    EXPECT_FALSE(b < a);
}

TEST(TimeoutKey, ToString)
{
    TimeoutKey k{"handshake", "PC-02"};
    EXPECT_EQ(k.toString(), "handshake: PC-02");
}

// ---------------------------------------------------------------------------
// TimeoutService — state queries
// ---------------------------------------------------------------------------

TEST(TimeoutService, InitiallyEmpty)
{
    TimeoutService svc;
    EXPECT_EQ(svc.activeCount(), 0u);
}

TEST(TimeoutService, StartedTimeoutIsActive)
{
    TimeoutService svc;
    TimeoutKey     key{"cat", "id"};
    svc.startTimeout(key, 500, [](const TimeoutKey &) {});
    EXPECT_TRUE(svc.isActive(key));
    EXPECT_EQ(svc.activeCount(), 1u);
    svc.cancelAll();
}

TEST(TimeoutService, CancelledTimeoutIsNotActive)
{
    TimeoutService svc;
    TimeoutKey     key{"cat", "id"};
    svc.startTimeout(key, 500, [](const TimeoutKey &) {});
    EXPECT_TRUE(svc.cancelTimeout(key));
    EXPECT_FALSE(svc.isActive(key));
    EXPECT_EQ(svc.activeCount(), 0u);
}

TEST(TimeoutService, CancelNonExistentReturnsFalse)
{
    TimeoutService svc;
    EXPECT_FALSE(svc.cancelTimeout({"nonexistent", "none"}));
}

// ---------------------------------------------------------------------------
// TimeoutService — callback behaviour
// ---------------------------------------------------------------------------

TEST(TimeoutService, CallbackFiredAfterTimeout)
{
    TimeoutService    svc;
    std::atomic<bool> fired{false};
    svc.startTimeout({"cat", "id"}, 100, [&](const TimeoutKey &) { fired.store(true); });
    std::this_thread::sleep_for(200ms);
    EXPECT_TRUE(fired.load());
}

TEST(TimeoutService, CancelledDoesNotFireCallback)
{
    TimeoutService    svc;
    std::atomic<bool> fired{false};
    TimeoutKey        key{"cat", "id"};
    svc.startTimeout(key, 100, [&](const TimeoutKey &) { fired.store(true); });
    svc.cancelTimeout(key);
    std::this_thread::sleep_for(200ms);
    EXPECT_FALSE(fired.load());
}

TEST(TimeoutService, RestartTimeoutCancelsPrevious)
{
    TimeoutService   svc;
    std::atomic<int> count{0};
    TimeoutKey       key{"cat", "id"};
    svc.startTimeout(key, 100, [&](const TimeoutKey &) { ++count; });
    svc.startTimeout(key, 100, [&](const TimeoutKey &) { ++count; });
    std::this_thread::sleep_for(250ms);
    EXPECT_EQ(count.load(), 1);
}

TEST(TimeoutService, MultipleTimeoutsFireIndependently)
{
    TimeoutService   svc;
    std::atomic<int> count{0};
    svc.startTimeout({"a", "1"}, 100, [&](const TimeoutKey &) { ++count; });
    svc.startTimeout({"b", "2"}, 100, [&](const TimeoutKey &) { ++count; });
    std::this_thread::sleep_for(250ms);
    EXPECT_EQ(count.load(), 2);
}

// ---------------------------------------------------------------------------
// TimeoutService — bulk cancel
// ---------------------------------------------------------------------------

TEST(TimeoutService, CancelCategoryRemovesAll)
{
    TimeoutService svc;
    svc.startTimeout({"request", "peer-a"}, 500, [](const TimeoutKey &) {});
    svc.startTimeout({"request", "peer-b"}, 500, [](const TimeoutKey &) {});
    EXPECT_EQ(svc.cancelCategory("request"), 2);
    EXPECT_EQ(svc.activeCount(), 0u);
}

TEST(TimeoutService, CancelCategoryLeavesOtherCategories)
{
    TimeoutService svc;
    svc.startTimeout({"request", "peer-a"}, 500, [](const TimeoutKey &) {});
    svc.startTimeout({"handshake", "peer-a"}, 500, [](const TimeoutKey &) {});
    EXPECT_EQ(svc.cancelCategory("request"), 1);
    EXPECT_TRUE(svc.isActive({"handshake", "peer-a"}));
    svc.cancelAll();
}

TEST(TimeoutService, CancelByIdentifierRemovesAll)
{
    TimeoutService svc;
    svc.startTimeout({"cat-a", "peer-x"}, 500, [](const TimeoutKey &) {});
    svc.startTimeout({"cat-b", "peer-x"}, 500, [](const TimeoutKey &) {});
    svc.startTimeout({"cat-a", "peer-y"}, 500, [](const TimeoutKey &) {});
    EXPECT_EQ(svc.cancelByIdentifier("peer-x"), 2);
    EXPECT_FALSE(svc.isActive({"cat-a", "peer-x"}));
    EXPECT_FALSE(svc.isActive({"cat-b", "peer-x"}));
    EXPECT_TRUE(svc.isActive({"cat-a", "peer-y"}));
    svc.cancelAll();
}

TEST(TimeoutService, CancelAllClearsEverything)
{
    TimeoutService svc;
    svc.startTimeout({"a", "1"}, 500, [](const TimeoutKey &) {});
    svc.startTimeout({"b", "2"}, 500, [](const TimeoutKey &) {});
    svc.startTimeout({"c", "3"}, 500, [](const TimeoutKey &) {});
    svc.cancelAll();
    EXPECT_EQ(svc.activeCount(), 0u);
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
        // svc destroyed here — destructor calls cancelAll()
    }
    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(fired.load());
}
