/*
  ==============================================================================
	Module:         CompatibilityCheckBenchmarks
	Description:    The pluggable secret and version checks
  ==============================================================================
*/

#include <benchmark/benchmark.h>

#include <string>
#include <string_view>

#include "Checks/SecretCompatibilityCheck.h"
#include "Checks/VersionCompatibilityCheck.h"

using namespace netlink;


namespace ValidationBenchmarks
{

static void BM_VersionCheck_IsCompatible(benchmark::State &state, std::string_view local, std::string_view remote)
{
	for (auto _ : state)
	{
		benchmark::DoNotOptimize(local);
		benchmark::DoNotOptimize(remote);
		benchmark::DoNotOptimize(VersionCompatibilityCheck::isCompatible(local, remote));
	}
}
BENCHMARK_CAPTURE(BM_VersionCheck_IsCompatible, Match, std::string_view{"1.4.2.1337"}, std::string_view{"1.4.9.12"});
BENCHMARK_CAPTURE(BM_VersionCheck_IsCompatible, Mismatch, std::string_view{"1.4.2.1337"}, std::string_view{"2.0.0.1"});
BENCHMARK_CAPTURE(BM_VersionCheck_IsCompatible, Malformed, std::string_view{"1.4.2.1337"}, std::string_view{"garbage"});


// One peer's full check cycle while `entries` other peers hold answers in the check
template <typename Check>
static void checkCycle(benchmark::State &state, const std::string &localValue, const std::string &remoteValue)
{
	Check check(localValue);

	for (int64_t i = 0; i < state.range(0); ++i)
		check.onRemoteDataReceived("resident-" + std::to_string(i), remoteValue);

	const std::string peer = "DESKTOP-4F2K9Q1";

	for (auto _ : state)
	{
		check.onRemoteDataReceived(peer, remoteValue);
		benchmark::DoNotOptimize(check.isReady(peer));
		benchmark::DoNotOptimize(check.evaluate(peer));
		check.reset(peer);
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}


static void BM_SecretCheck_Cycle(benchmark::State &state)
{
	checkCycle<SecretCompatibilityCheck>(state, "a-shared-application-secret", "a-shared-application-secret");
}
BENCHMARK(BM_SecretCheck_Cycle)->ArgName("residents")->Arg(0)->Arg(64)->Arg(1024);


static void BM_VersionCheck_Cycle(benchmark::State &state)
{
	checkCycle<VersionCompatibilityCheck>(state, "1.4.2.1337", "1.4.9.12");
}
BENCHMARK(BM_VersionCheck_Cycle)->ArgName("residents")->Arg(0)->Arg(64)->Arg(1024);

} // namespace ValidationBenchmarks
