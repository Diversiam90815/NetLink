/*
  ==============================================================================
	Module:         DiscoveryBenchmarks
	Description:    The bookkeeping every received discovery announcement triggers
  ==============================================================================
*/

#include <benchmark/benchmark.h>

#include <string>

#include "BenchUtil.h"
#include "Discovery/DiscoveryRegistry.h"

using namespace netlink;
using netlink::discovery::DiscoveryRegistry;


namespace DiscoveryBenchmarks
{

static DiscoveryEndpoint makeEndpoint(const int64_t index)
{
	return DiscoveryEndpoint{.IPAddress	  = net::IPv4Address::fromHostOrder(0x0A000000u + static_cast<uint32_t>(index)),
							 .port		  = 50000 + static_cast<int>(index % 10000),
							 .displayName = "peer-" + std::to_string(index)};
}


// Time: handling one re-announcement of a known peer (the steady state of discovery) with `known_peers` peers on the LAN.
// The worst case: the announcing peer is the last one in the registry.
static void BM_DiscoveryRegistry_Announcement(benchmark::State &state)
{
	DiscoveryRegistry registry;
	for (int64_t i = 0; i < state.range(0); ++i)
		registry.addOrUpdate(makeEndpoint(i));

	const auto announcing = makeEndpoint(state.range(0) - 1);

	for (auto _ : state)
		benchmark::DoNotOptimize(registry.addOrUpdate(announcing));
}
BENCHMARK(BM_DiscoveryRegistry_Announcement)->ArgName("known_peers")->Arg(8)->Arg(64)->Arg(512)->Arg(4096)->Unit(benchmark::kNanosecond);

} // namespace DiscoveryBenchmarks
