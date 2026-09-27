/*
  ==============================================================================
	Module:         TimeoutServiceBenchmarks
	Description:    Arming, re-arming and cancelling timeouts with a populated
					service, contention, and how late timeouts actually fire.
					Load: tens of thousands of active timeouts, mass expiry.
  ==============================================================================
*/

#include <benchmark/benchmark.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "BenchUtil.h"
#include "TimeoutService/TimeoutService.h"


namespace TimeoutBenchmarks
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


static void populate(TimeoutService &service, const std::vector<TimeoutKey> &keys)
{
	for (const auto &key : keys)
		service.startTimeout(key, NeverMs, [](const TimeoutKey &) {});
}


// Re-arming a key that is already active (the service replaces the entry), with `entries` timeouts active
static void BM_TimeoutService_Restart(benchmark::State &state)
{
	const auto	   keys = makeKeys(static_cast<size_t>(state.range(0)));
	TimeoutService service;
	populate(service, keys);

	size_t next = 0;

	for (auto _ : state)
	{
		service.startTimeout(keys[next], NeverMs, [](const TimeoutKey &) {});
		next = (next + 1) % keys.size();
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_TimeoutService_Restart)->Apply(bench::populations);


// Arm a fresh timeout and cancel it again: the arm/disarm cycle of every connection and validation step
static void BM_TimeoutService_StartCancel(benchmark::State &state)
{
	TimeoutService service;
	populate(service, makeKeys(static_cast<size_t>(state.range(0))));

	const TimeoutKey key{.category = "invitation", .identifier = "bench-peer"};

	for (auto _ : state)
	{
		service.startTimeout(key, NeverMs, [](const TimeoutKey &) {});
		benchmark::DoNotOptimize(service.cancelTimeout(key));
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_TimeoutService_StartCancel)->Apply(bench::populations)->Arg(16'384)->Arg(65'536)->Unit(benchmark::kMicrosecond);


// Arms one timeout per category for a peer, then cancels them all by identifier (a peer going away)
static void BM_TimeoutService_CancelByIdentifier(benchmark::State &state)
{
	TimeoutService service;
	populate(service, makeKeys(static_cast<size_t>(state.range(0))));

	std::vector<TimeoutKey> leaving;
	for (const auto *category : Categories)
		leaving.push_back({.category = category, .identifier = "leaving-peer"});

	for (auto _ : state)
	{
		for (const auto &key : leaving)
			service.startTimeout(key, NeverMs, [](const TimeoutKey &) {});

		benchmark::DoNotOptimize(service.cancelByIdentifier("leaving-peer"));
	}

	state.SetLabel("5 armed + 1 cancel per iteration");
}
BENCHMARK(BM_TimeoutService_CancelByIdentifier)->Apply(bench::populations);


// Arms a burst of timeouts in one category, then cancels the category
static void BM_TimeoutService_CancelCategory(benchmark::State &state)
{
	TimeoutService service;
	populate(service, makeKeys(static_cast<size_t>(state.range(0))));

	std::vector<TimeoutKey> burst;
	for (int i = 0; i < 8; ++i)
		burst.push_back({.category = "burst", .identifier = "burst-" + std::to_string(i)});

	for (auto _ : state)
	{
		for (const auto &key : burst)
			service.startTimeout(key, NeverMs, [](const TimeoutKey &) {});

		benchmark::DoNotOptimize(service.cancelCategory("burst"));
	}

	state.SetLabel("8 armed + 1 cancel per iteration");
}
BENCHMARK(BM_TimeoutService_CancelCategory)->Apply(bench::populations);


// One service shared by 1..8 threads, each arming and cancelling its own key
static std::unique_ptr<TimeoutService> gSharedService;

static void createSharedService(const benchmark::State &)
{
	gSharedService = std::make_unique<TimeoutService>();
	populate(*gSharedService, makeKeys(64));
}

static void destroySharedService(const benchmark::State &)
{
	gSharedService.reset();
}

static void BM_TimeoutService_StartCancel_Contended(benchmark::State &state)
{
	const TimeoutKey key{.category = "invitation", .identifier = "thread-" + std::to_string(state.thread_index())};

	for (auto _ : state)
	{
		gSharedService->startTimeout(key, NeverMs, [](const TimeoutKey &) {});
		benchmark::DoNotOptimize(gSharedService->cancelTimeout(key));
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_TimeoutService_StartCancel_Contended)->Setup(createSharedService)->Teardown(destroySharedService)->ThreadRange(1, 8)->UseRealTime();


// Time from arming until the callback ran, with `active` other timeouts armed.
// overshoot_us: how late it fired compared to the requested duration.
static void BM_TimeoutService_FireLatency(benchmark::State &state)
{
	const int				 timeoutMs = static_cast<int>(state.range(0));
	TimeoutService			 service;
	populate(service, makeKeys(static_cast<size_t>(state.range(1)), "background-"));

	bench::CompletionCounter fired;
	bench::Clock::time_point firedAt;
	uint64_t				 expected	= 0;
	double					 overshootUs = 0.0;

	const TimeoutKey		 key{.category = "handshake", .identifier = "bench-peer"};

	for (auto _ : state)
	{
		const auto start = bench::Clock::now();

		service.startTimeout(key, timeoutMs,
							 [&](const TimeoutKey &)
							 {
								 firedAt = bench::Clock::now();
								 fired.notify();
							 });

		if (!fired.waitFor(++expected))
		{
			state.SkipWithError("Timeout never fired");
			break;
		}

		const auto elapsed = std::chrono::duration<double>(firedAt - start);
		state.SetIterationTime(elapsed.count());
		overshootUs += std::chrono::duration<double, std::micro>(elapsed - std::chrono::milliseconds{timeoutMs}).count();
	}

	state.counters["overshoot_us"] = benchmark::Counter(overshootUs, benchmark::Counter::kAvgIterations);
}
BENCHMARK(BM_TimeoutService_FireLatency)
	->ArgNames({"timeoutMs", "active"})
	->Args({0, 0})
	->Args({1, 0})
	->Args({5, 0})
	->Args({10, 0})
	->Args({0, 1024})
	->Args({0, 16'384})
	->Args({0, 65'536})
	->UseManualTime()
	->Unit(benchmark::kMicrosecond);


// Many timeouts come due at once (every peer of a large LAN timing out together). Timed until every callback ran.
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
		if (!fired.waitFor(expected, bench::LoadTimeout))
		{
			state.SkipWithError("Not every timeout fired");
			break;
		}

		state.SetIterationTime(bench::secondsSince(start));
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * count));
}
BENCHMARK(BM_TimeoutService_MassExpiry)->ArgName("timeouts")->Arg(1'000)->Arg(10'000)->Arg(30'000)->UseManualTime()->Unit(benchmark::kMillisecond);

} // namespace TimeoutBenchmarks
