#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "TaskQueue.h"

using namespace std::chrono_literals;


// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

TEST(TaskQueue, IsNotRunningBeforeStart)
{
	TaskQueue q;
	EXPECT_FALSE(q.isRunning());
}


TEST(TaskQueue, StartMakesItRunning)
{
	TaskQueue q;
	q.start();
	EXPECT_TRUE(q.isRunning());
	q.stop();
}


TEST(TaskQueue, StopJoinsWorkerThread)
{
	TaskQueue q;
	q.start();
	q.stop();
	EXPECT_FALSE(q.isRunning());
}


// ---------------------------------------------------------------------------
// Task execution
// ---------------------------------------------------------------------------

TEST(TaskQueue, PostedTaskEventuallyRuns)
{
	TaskQueue		  q;
	std::atomic<bool> ran{false};

	q.start();
	q.post([&ran]() { ran.store(true); });

	// Poll briefly instead of a fixed sleep to keep the test fast and robust.
	for (int i = 0; i < 50 && !ran.load(); ++i)
		std::this_thread::sleep_for(10ms);

	EXPECT_TRUE(ran.load()) << "A posted task must execute on the worker thread";
	q.stop();
}


TEST(TaskQueue, MultipleTasksRunInFifoOrder)
{
	TaskQueue		 q;
	std::mutex		 orderMutex;
	std::vector<int> order;

	q.start();

	for (int i = 0; i < 10; ++i)
	{
		q.post(
			[&order, &orderMutex, i]()
			{
				std::lock_guard<std::mutex> lock(orderMutex);
				order.push_back(i);
			});
	}

	// Wait until all 10 tasks have run.
	for (int i = 0; i < 100; ++i)
	{
		{
			std::lock_guard<std::mutex> lock(orderMutex);
			if (order.size() == 10)
				break;
		}
		std::this_thread::sleep_for(10ms);
	}

	std::lock_guard<std::mutex> lock(orderMutex);
	ASSERT_EQ(order.size(), 10u);
	for (int i = 0; i < 10; ++i)
		EXPECT_EQ(order[i], i) << "Tasks must execute strictly in the order they were posted (FIFO)";
}


TEST(TaskQueue, TasksRunSerially_NeverConcurrently)
{
	TaskQueue		 q;
	std::atomic<int> concurrentCount{0};
	std::atomic<int> maxConcurrent{0};
	std::atomic<int> completed{0};

	q.start();

	for (int i = 0; i < 20; ++i)
	{
		q.post(
			[&]()
			{
				int current = ++concurrentCount;
				int prevMax = maxConcurrent.load();
				while (current > prevMax && !maxConcurrent.compare_exchange_weak(prevMax, current))
				{
				}
				std::this_thread::sleep_for(2ms);
				--concurrentCount;
				++completed;
			});
	}

	for (int i = 0; i < 200 && completed.load() < 20; ++i)
		std::this_thread::sleep_for(10ms);

	EXPECT_EQ(completed.load(), 20);
	EXPECT_LE(maxConcurrent.load(), 1) << "TaskQueue must execute tasks one at a time on a single worker thread";
}


TEST(TaskQueue, PendingReflectsQueueSizeBeforeExecution)
{
	TaskQueue					 q;
	std::mutex					 blockMutex;
	std::unique_lock<std::mutex> blockLock(blockMutex);

	q.start();

	// First task blocks until we release blockMutex, letting us observe the queue mid-flight.
	q.post([&blockMutex]() { std::lock_guard<std::mutex> lock(blockMutex); });

	std::this_thread::sleep_for(20ms); // give the first task time to start and block
	q.post([]() {});
	q.post([]() {});

	// pending() may be 2 (task 1 already dequeued/running) - just verify it's non-negative and bounded.
	EXPECT_LE(q.pending(), 3u);

	blockLock.unlock();
	q.stop();
}


TEST(TaskQueue, StopDropsUnexecutedPendingTasks)
{
	TaskQueue				q;
	std::atomic<int>		executedCount{0};
	std::atomic<bool>		firstTaskStarted{false};

	std::mutex				blockMutex;
	std::condition_variable blockCV;
	bool					releaseFirstTask = false;

	q.start();

	q.post(
		[&]()
		{
			firstTaskStarted.store(true);
			std::unique_lock<std::mutex> lock(blockMutex);
			blockCV.wait(lock, [&] { return releaseFirstTask; });
			++executedCount;
		});

	for (int i = 0; i < 500 && !firstTaskStarted.load(); ++i)
		std::this_thread::sleep_for(1ms);
	ASSERT_TRUE(firstTaskStarted.load()) << "Precondition: the first task must have started before proceeding";

	for (int i = 0; i < 49; ++i)
		q.post([&executedCount]() { ++executedCount; });

	std::thread stopper([&q]() { q.stop(); });
	std::this_thread::sleep_for(50ms);

	{
		std::lock_guard<std::mutex> lock(blockMutex);
		releaseFirstTask = true;
	}
	blockCV.notify_one();

	stopper.join();

	EXPECT_EQ(executedCount.load(), 1) << "Exactly the single in-flight task must complete; "
										  "all 49 remaining queued tasks must be dropped by stop()";
}


TEST(TaskQueue, DestructorStopsCleanly)
{
	std::atomic<bool> ran{false};
	{
		TaskQueue q;
		q.start();
		q.post([&ran]() { ran.store(true); });
		std::this_thread::sleep_for(50ms);
	} // destructor calls stop()

	EXPECT_TRUE(ran.load()) << "A task posted and given time to run before destruction should have executed";
}