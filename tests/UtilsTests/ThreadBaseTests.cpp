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


namespace UtilsTests
{

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

TEST(ThreadBase, IsNotRunningBeforeStart)
{
	EventCounterThread t;
	EXPECT_FALSE(t.isRunning()) << "A freshly constructed thread must not be running until start() is called";
}


TEST(ThreadBase, IsRunningAfterStart)
{
	EventCounterThread t;
	t.start();
	EXPECT_TRUE(t.isRunning()) << "isRunning() must return true immediately after start() launches the worker thread";
	t.stop();
}


TEST(ThreadBase, IsStoppedAfterStop)
{
	EventCounterThread t;
	t.start();
	t.stop();
	EXPECT_FALSE(t.isRunning()) << "isRunning() must return false after stop() joins the worker thread";
}


TEST(ThreadBase, StopWhileStoppedIsNoop)
{
	EventCounterThread t;
	// Never started — stop() must not crash or deadlock
	EXPECT_NO_THROW(t.stop()) << "Calling stop() on a thread that was never started must not throw or crash";
	EXPECT_FALSE(t.isRunning()) << "isRunning() must still be false after a redundant stop() call";
}


TEST(ThreadBase, StartWhileRunningIsNoop)
{
	EventCounterThread t;
	t.start();
	int countBefore = t.loopCount.load();
	t.start(); // second call must be a no-op — must not spawn a second thread
	t.triggerEvent();
	std::this_thread::sleep_for(100ms);
	int countAfter = t.loopCount.load();
	EXPECT_EQ(countAfter - countBefore, 1) << "Calling start() on an already-running thread must not spawn a second worker; "
											  "a single triggerEvent() must advance loopCount by exactly 1";
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
	EXPECT_GT(t.loopCount.load(), before) << "triggerEvent() must wake the blocked thread, causing loopCount to increase";
	t.stop();
}


TEST(ThreadBase, StopWakesBlockedThread)
{
	// The thread blocks inside waitForEvent(50 ms poll).
	// stop() must unblock it and join within a reasonable time.
	EventCounterThread t;
	t.start();
	auto before = std::chrono::steady_clock::now();
	t.stop();
	auto elapsed = std::chrono::steady_clock::now() - before;
	EXPECT_FALSE(t.isRunning()) << "stop() must set isRunning() to false";
	EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 200) << "stop() must unblock and join the thread within one poll cycle (< 200 ms)";
}


TEST(ThreadBase, LoopCountDoesNotAdvanceAfterStop)
{
	EventCounterThread t;
	t.start();
	t.triggerEvent();
	std::this_thread::sleep_for(100ms);
	t.stop();
	int countAtStop = t.loopCount.load();
	// No further events — loopCount must freeze
	std::this_thread::sleep_for(100ms);
	EXPECT_EQ(t.loopCount.load(), countAtStop) << "After stop(), the worker thread must no longer be running so loopCount must not increase further";
}

} // namespace UtilsTests
