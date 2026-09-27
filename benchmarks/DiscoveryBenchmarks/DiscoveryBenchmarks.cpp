/*
  ==============================================================================
	Module:         DiscoveryBenchmarks
	Description:    Known-peer bookkeeping driven by every announcement, and the
					encoding of the announcement itself
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


static void populate(DiscoveryRegistry &registry, const int64_t count)
{
	for (int64_t i = 0; i < count; ++i)
		registry.addOrUpdate(makeEndpoint(i));
}


// Re-announcement of a known peer (the steady state of discovery). Worst case: the last peer of the list.
static void BM_DiscoveryRegistry_Refresh(benchmark::State &state)
{
	DiscoveryRegistry registry;
	populate(registry, state.range(0));

	const auto peer = makeEndpoint(state.range(0) - 1);

	for (auto _ : state)
		benchmark::DoNotOptimize(registry.addOrUpdate(peer));
}
BENCHMARK(BM_DiscoveryRegistry_Refresh)->Apply(bench::populations);


static void BM_DiscoveryRegistry_FindByIP(benchmark::State &state)
{
	DiscoveryRegistry registry;
	populate(registry, state.range(0));

	const auto ip = makeEndpoint(state.range(0) - 1).IPAddress;

	for (auto _ : state)
		benchmark::DoNotOptimize(registry.findByIP(ip));
}
BENCHMARK(BM_DiscoveryRegistry_FindByIP)->Apply(bench::populations);


// A peer appears and leaves again
static void BM_DiscoveryRegistry_AddRemove(benchmark::State &state)
{
	DiscoveryRegistry registry;
	populate(registry, state.range(0));

	const auto peer = makeEndpoint(state.range(0));

	for (auto _ : state)
	{
		registry.addOrUpdate(peer);
		benchmark::DoNotOptimize(registry.remove(peer));
	}
}
BENCHMARK(BM_DiscoveryRegistry_AddRemove)->Apply(bench::populations);


// The expiry scan the discovery thread runs every cycle, with nobody stale
static void BM_DiscoveryRegistry_RemoveStale(benchmark::State &state)
{
	DiscoveryRegistry registry;
	populate(registry, state.range(0));

	for (auto _ : state)
		benchmark::DoNotOptimize(registry.removeStale(std::chrono::hours{1}));

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * state.range(0));
}
BENCHMARK(BM_DiscoveryRegistry_RemoveStale)->Apply(bench::populations);


static void BM_DiscoveryRegistry_Snapshot(benchmark::State &state)
{
	DiscoveryRegistry registry;
	populate(registry, state.range(0));

	for (auto _ : state)
		benchmark::DoNotOptimize(registry.snapshot());

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * state.range(0));
}
BENCHMARK(BM_DiscoveryRegistry_Snapshot)->Apply(bench::populations);


// The announcement datagram: encoded by the sender, parsed by every receiver
static void BM_DiscoveryEndpoint_Encode(benchmark::State &state)
{
	const auto endpoint = makeEndpoint(123);

	for (auto _ : state)
	{
		std::string encoded = nlohmann::json(endpoint).dump();
		benchmark::DoNotOptimize(encoded);
	}
}
BENCHMARK(BM_DiscoveryEndpoint_Encode);


static void BM_DiscoveryEndpoint_Parse(benchmark::State &state)
{
	const std::string encoded = nlohmann::json(makeEndpoint(123)).dump();

	for (auto _ : state)
	{
		auto endpoint = nlohmann::json::parse(encoded).get<DiscoveryEndpoint>();
		benchmark::DoNotOptimize(endpoint);
	}
}
BENCHMARK(BM_DiscoveryEndpoint_Parse);

} // namespace DiscoveryBenchmarks
