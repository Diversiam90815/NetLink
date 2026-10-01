/*
  ==============================================================================
	Module:         TimeoutServiceBenchmarks
	Description:    Arming, cancelling and firing timeouts, depending on how many
					are active, and how precisely they fire
  ==============================================================================
*/

#include <benchmark/benchmark.h>

#include <array>
#include <string>
#include <vector>

#include "BenchUtil.h"
#include "TimeoutService/TimeoutService.h"


namespace UtilsBenchmarks
{

// Long enough that nothing expires while a benchmark runs
static constexpr int						   NeverMs	  = 3'600'000;

// The categories the library arms
static constexpr std::array<const char *, 5> Categories = {"handshake", "secret_request", "version_request", "invitation", "ready_flag"};


static std::vector<TimeoutKey>				   makeKeys(const size_t count, const std::string &prefix = "peer-")
{
	std::vector<TimeoutKey> keys;
	keys.reserve(count);

	for (size_t i = 0; i < count; ++i)
		keys.push_back({.category = Categories[i % Categories.size()], .identifier = prefix + std::to_string(i)});

	return keys;
}


static void populate(TimeoutService &service, const size_t count)
{
	for (const auto &key : makeKeys(count, "background-"))
		service.startTimeout(key, NeverMs, [](const TimeoutKey &) {});
}


// Time: arming one timeout and cancelling it again (every connection and validation step does this), with `active` others
static void BM_TimeoutService_StartCancel(benchmark::State &state)
{
	TimeoutService service;
	populate(service, static_cast<size_t>(state.range(0)));

	const TimeoutKey key{.category = "invitation", .identifier = "bench-peer"};

	for (auto _ : state)
	{
		service.startTimeout(key, NeverMs, [](const TimeoutKey &) {});
		benchmark::DoNotOptimize(service.cancelTimeout(key));
	}
}
BENCHMARK(BM_TimeoutService_StartCancel)->ArgName("active")->Arg(64)->Arg(4096)->Arg(65'536)->MeasureProcessCPUTime()->Unit(benchmark::kMicrosecond);


// Time: an immediately due timeout from arming until its callback ran, with `active` others the worker has to look through
static void BM_TimeoutService_FireLatency(benchmark::State &state)
{
	TimeoutService service;
	populate(service, static_cast<size_t>(state.range(0)));

	bench::CompletionCounter fired;
	uint64_t				 expected = 0;
	const TimeoutKey		 key{.category = "handshake", .identifier = "bench-peer"};

	for (auto _ : state)
	{
		const auto start = bench::Clock::now();
		service.startTimeout(key, 0, [&fired](const TimeoutKey &) { fired.notify(); });

		if (!fired.waitFor(++expected))
		{
			state.SkipWithError("The timeout never fired");
			break;
		}

		state.SetIterationTime(bench::secondsSince(start));
	}
}
BENCHMARK(BM_TimeoutService_FireLatency)->ArgName("active")->Arg(0)->Arg(1024)->Arg(65'536)->UseManualTime()->MeasureProcessCPUTime()->Unit(benchmark::kMicrosecond);


// Time: a short timeout from arming until its callback ran. late_ms: how much later than requested it fired, which is
// bounded by the operating system's timer resolution (about 15.6 ms on Windows by default).
static void BM_TimeoutService_TimerResolution(benchmark::State &state)
{
	const auto				 timeout = std::chrono::milliseconds{state.range(0)};
	TimeoutService			 service;
	bench::CompletionCounter fired;
	uint64_t				 expected = 0;
	double					 lateMs	  = 0.0;
	const TimeoutKey		 key{.category = "handshake", .identifier = "bench-peer"};

	for (auto _ : state)
	{
		const auto start = bench::Clock::now();
		service.startTimeout(key, static_cast<int>(timeout.count()), [&fired](const TimeoutKey &) { fired.notify(); });

		if (!fired.waitFor(++expected))
		{
			state.SkipWithError("The timeout never fired");
			break;
		}

		const auto elapsed = std::chrono::duration<double, std::milli>(bench::Clock::now() - start);
		state.SetIterationTime(elapsed.count() / 1000.0);
		lateMs += (elapsed - timeout).count();
	}

	state.counters["late_ms"] = benchmark::Counter(lateMs, benchmark::Counter::kAvgIterations);
}
BENCHMARK(BM_TimeoutService_TimerResolution)->ArgName("timeout_ms")->Arg(1)->Arg(5)->Arg(10)->UseManualTime()->MeasureProcessCPUTime()->Unit(benchmark::kMillisecond);


// Time: `timeouts` timeouts that come due at once (every peer of a large LAN timing out together) until every callback ran
static void BM_TimeoutService_MassExpiry(benchmark::State &state)
{
	const auto				 count = static_cast<size_t>(state.range(0));
	const auto				 keys  = makeKeys(count);
	bench::CompletionCounter fired;
	uint64_t				 expected = 0;

	for (auto _ : state)
	{
		TimeoutService service; // fresh per round; its thread start and shutdown are outside the measured time
		const auto	   start = bench::Clock::now();

		for (const auto &key : keys)
			service.startTimeout(key, 0, [&fired](const TimeoutKey &) { fired.notify(); });

		expected += count;
		if (!fired.waitFor(expected))
		{
			state.SkipWithError("Not every timeout fired");
			break;
		}

		state.SetIterationTime(bench::secondsSince(start));
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * count));
}
BENCHMARK(BM_TimeoutService_MassExpiry)->ArgName("timeouts")->Arg(1000)->Arg(10'000)->Arg(30'000)->UseManualTime()->MeasureProcessCPUTime()->Unit(benchmark::kMillisecond);

} // namespace UtilsBenchmarks
