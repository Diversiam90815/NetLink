/*
  ==============================================================================
	Module:         TaskQueueBenchmarks
	Description:    Serial worker queue used by ConnectionService and NetLinkCore.
					Load: a million tasks from several producers at once.
  ==============================================================================
*/

#include <benchmark/benchmark.h>

#include <array>
#include <atomic>
#include <latch>
#include <memory>
#include <thread>
#include <vector>

#include "BenchUtil.h"
#include "Util/TaskQueue.h"


namespace QueueBenchmarks
{

// Producers pause once this many tasks are pending, so a fast producer cannot grow the queue without bound
static constexpr size_t			  MaxBacklog = 4096;

// Shared by all threads of a multi-threaded run, created before and destroyed after the threads
static std::unique_ptr<TaskQueue> gSharedQueue;
static std::atomic<uint64_t>	  gExecuted{0};

static void						  startSharedQueue(const benchmark::State &)
{
	gExecuted.store(0);
	gSharedQueue = std::make_unique<TaskQueue>();
	gSharedQueue->start();
}

static void stopSharedQueue(const benchmark::State &)
{
	gSharedQueue.reset();
}


static void throttle(const TaskQueue &queue, const uint64_t posted)
{
	if (posted % 1024 == 0)
	{
		while (queue.pending() > MaxBacklog)
			std::this_thread::yield();
	}
}


// Sustained posting rate from 1..8 threads into one queue while the worker executes
static void BM_TaskQueue_Post(benchmark::State &state)
{
	uint64_t posted = 0;

	for (auto _ : state)
	{
		gSharedQueue->post([] { gExecuted.fetch_add(1, std::memory_order_relaxed); });
		throttle(*gSharedQueue, ++posted);
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_TaskQueue_Post)->Setup(startSharedQueue)->Teardown(stopSharedQueue)->ThreadRange(1, 8)->UseRealTime();


// Cost of the captured state: small captures fit std::function's inline buffer, large ones allocate
template <size_t CaptureSize>
static void BM_TaskQueue_PostCapture(benchmark::State &state)
{
	std::array<uint8_t, CaptureSize> capture{};
	std::atomic<uint64_t>			 sink{0};
	uint64_t						 posted = 0;

	TaskQueue						 queue; // declared last: stopped before the state its tasks reference
	queue.start();

	for (auto _ : state)
	{
		queue.post([capture, &sink] { sink.fetch_add(capture[0], std::memory_order_relaxed); });
		throttle(queue, ++posted);
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK_TEMPLATE(BM_TaskQueue_PostCapture, 8)->UseRealTime();
BENCHMARK_TEMPLATE(BM_TaskQueue_PostCapture, 256)->UseRealTime();


// A burst of tasks posted and executed completely
static void BM_TaskQueue_Drain(benchmark::State &state)
{
	const auto				 batch = static_cast<size_t>(state.range(0));
	std::atomic<uint64_t>	 executed{0};
	bench::CompletionCounter batches;
	uint64_t				 posted = 0;

	TaskQueue				 queue; // declared last: stopped before the state its tasks reference
	queue.start();

	for (auto _ : state)
	{
		for (size_t i = 0; i + 1 < batch; ++i)
			queue.post([&executed] { executed.fetch_add(1, std::memory_order_relaxed); });

		// FIFO: once the last task ran, the whole batch did
		queue.post([&batches] { batches.notify(); });

		if (!batches.waitFor(++posted))
		{
			state.SkipWithError("TaskQueue did not drain in time");
			break;
		}
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * batch));
}
BENCHMARK(BM_TaskQueue_Drain)->ArgName("batch")->Arg(1)->Arg(64)->Arg(1024)->UseRealTime();


// One million tasks posted by 1..8 producers at once, without throttling. Timed until the worker executed all of them.
static void BM_TaskQueue_Load_Burst(benchmark::State &state)
{
	constexpr uint64_t		 Total		 = 1'000'000;
	const auto				 producers	 = static_cast<size_t>(state.range(0));
	const uint64_t			 perProducer = Total / producers;

	std::atomic<uint64_t>	 executed{0};
	bench::CompletionCounter done;
	uint64_t				 target = 0;
	uint64_t				 rounds = 0;

	TaskQueue				 queue; // declared last: stopped before the state its tasks reference
	queue.start();

	for (auto _ : state)
	{
		target += perProducer * producers;

		std::latch				 go(1);
		std::vector<std::thread> threads;

		for (size_t p = 0; p < producers; ++p)
		{
			threads.emplace_back(
				[&]
				{
					go.wait();
					for (uint64_t i = 0; i < perProducer; ++i)
						queue.post(
							[&executed, &done, target]
							{
								if (executed.fetch_add(1, std::memory_order_relaxed) + 1 == target)
									done.notify();
							});
				});
		}

		const auto start = bench::Clock::now();
		go.count_down();

		for (auto &thread : threads)
			thread.join();

		if (!done.waitFor(++rounds, bench::LoadTimeout))
		{
			state.SkipWithError("The queue did not drain");
			break;
		}

		state.SetIterationTime(bench::secondsSince(start));
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * perProducer * producers));
}
BENCHMARK(BM_TaskQueue_Load_Burst)->ArgName("producers")->Arg(1)->Arg(4)->Arg(8)->UseManualTime()->Unit(benchmark::kMillisecond);

} // namespace QueueBenchmarks
