#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include "ThreadBase.h"

using namespace std::chrono_literals;

// Minimal concrete subclass: counts how many times the event loop fires.
class EventCounterThread : public ThreadBase
{
public:
    std::atomic<int> loopCount{0};

protected:
    void run() override
    {
        while (waitForEvent(50)) // 50 ms poll keeps the thread responsive
            ++loopCount;
    }
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

TEST(ThreadBase, IsNotRunningBeforeStart)
{
    EventCounterThread t;
    EXPECT_FALSE(t.isRunning());
}

TEST(ThreadBase, IsRunningAfterStart)
{
    EventCounterThread t;
    t.start();
    EXPECT_TRUE(t.isRunning());
    t.stop();
}

TEST(ThreadBase, IsStoppedAfterStop)
{
    EventCounterThread t;
    t.start();
    t.stop();
    EXPECT_FALSE(t.isRunning());
}

TEST(ThreadBase, StopWhileStoppedIsNoop)
{
    EventCounterThread t;
    // Never started — stop() must not crash
    EXPECT_NO_THROW(t.stop());
    EXPECT_FALSE(t.isRunning());
}

TEST(ThreadBase, StartWhileRunningIsNoop)
{
    EventCounterThread t;
    t.start();
    int countBefore = t.loopCount.load();
    t.start(); // second call should be a no-op
    t.triggerEvent();
    std::this_thread::sleep_for(100ms);
    // loopCount should have advanced by exactly 1 (one event, not two threads each counting)
    int countAfter = t.loopCount.load();
    EXPECT_EQ(countAfter - countBefore, 1);
    t.stop();
}

// ---------------------------------------------------------------------------
// Event signaling
// ---------------------------------------------------------------------------

TEST(ThreadBase, TriggerEventWakesThread)
{
    EventCounterThread t;
    t.start();
    int before = t.loopCount.load();
    t.triggerEvent();
    std::this_thread::sleep_for(150ms);
    EXPECT_GT(t.loopCount.load(), before);
    t.stop();
}

TEST(ThreadBase, StopWakesBlockedThread)
{
    // The thread is blocked inside waitForEvent(50ms poll).
    // stop() must unblock it and join within a reasonable timeout.
    EventCounterThread t;
    t.start();
    auto before = std::chrono::steady_clock::now();
    t.stop();
    auto elapsed = std::chrono::steady_clock::now() - before;
    EXPECT_FALSE(t.isRunning());
    // Should join well within 200 ms (one poll cycle + overhead)
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 200);
}

TEST(ThreadBase, LoopCountDoesNotAdvanceAfterStop)
{
    EventCounterThread t;
    t.start();
    t.triggerEvent();
    std::this_thread::sleep_for(100ms);
    t.stop();
    int countAtStop = t.loopCount.load();
    // No more events — count must not increase after stop()
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(t.loopCount.load(), countAtStop);
}
