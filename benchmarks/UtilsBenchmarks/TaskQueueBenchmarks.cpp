/*
  ==============================================================================
	Module:         TaskQueueBenchmarks
	Description:    The serial worker queue that runs ConnectionService's and
					NetLinkCore's event handling
  ==============================================================================
*/

#include <benchmark/benchmark.h>

#include <atomic>
#include <latch>
#include <thread>
#include <vector>

#include "BenchUtil.h"
#include "Util/TaskQueue.h"


namespace UtilsBenchmarks
{

// Time: one task from post() until it ran on the worker thread
static void BM_TaskQueue_Latency(benchmark::State &state)
{
	bench::CompletionCounter done;
	uint64_t				 expected = 0;

	TaskQueue				 queue; // declared last: stopped before the state its tasks reference
	queue.start();

	for (auto _ : state)
	{
		queue.post([&done] { done.notify(); });

		if (!done.waitFor(++expected))
		{
			state.SkipWithError("The task never ran");
			break;
		}
	}
}
BENCHMARK(BM_TaskQueue_Latency)->UseRealTime()->MeasureProcessCPUTime()->Unit(benchmark::kMicrosecond);


// Time: 100k tasks posted by `producers` threads at once until the worker executed all of them. Read items_per_second as tasks/s.
static void BM_TaskQueue_Throughput(benchmark::State &state)
{
	constexpr uint64_t		 Tasks		 = 100'000;
	const auto				 producers	 = static_cast<uint64_t>(state.range(0));
	const uint64_t			 perProducer = Tasks / producers;
	const uint64_t			 perRound	 = perProducer * producers;

	std::atomic<uint64_t>	 executed{0};
	bench::CompletionCounter rounds;
	uint64_t				 round = 0;

	TaskQueue				 queue; // declared last: stopped before the state its tasks reference
	queue.start();

	for (auto _ : state)
	{
		const uint64_t target = ++round * perRound;

		std::latch				 go(1);
		std::vector<std::thread> threads;

		for (uint64_t p = 0; p < producers; ++p)
		{
			threads.emplace_back(
				[&]
				{
					go.wait();
					for (uint64_t i = 0; i < perProducer; ++i)
						queue.post(
							[&executed, &rounds, target]
							{
								if (executed.fetch_add(1, std::memory_order_relaxed) + 1 == target)
									rounds.notify();
							});
				});
		}

		const auto start = bench::Clock::now();
		go.count_down();

		for (auto &thread : threads)
			thread.join();

		if (!rounds.waitFor(round))
		{
			state.SkipWithError("The queue did not drain");
			break;
		}

		state.SetIterationTime(bench::secondsSince(start));
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * perRound));
}
BENCHMARK(BM_TaskQueue_Throughput)->ArgName("producers")->Arg(1)->Arg(4)->Arg(8)->UseManualTime()->MeasureProcessCPUTime()->Unit(benchmark::kMillisecond);

} // namespace UtilsBenchmarks
