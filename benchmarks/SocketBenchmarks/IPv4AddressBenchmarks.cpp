/*
  ==============================================================================
	Module:         IPv4AddressBenchmarks
	Description:    Address parsing, formatting and comparison
  ==============================================================================
*/

#include <benchmark/benchmark.h>

#include <map>
#include <string>
#include <string_view>

#include "BenchUtil.h"
#include "Socket/SocketTypes.h"

using namespace netlink::net;


namespace SocketBenchmarks
{

static void BM_IPv4Address_Parse(benchmark::State &state, std::string_view text)
{
	for (auto _ : state)
	{
		benchmark::DoNotOptimize(text); // keeps the constexpr parser from folding the literal
		auto address = IPv4Address::parse(text);
		benchmark::DoNotOptimize(address);
	}
}
BENCHMARK_CAPTURE(BM_IPv4Address_Parse, Valid, std::string_view{"192.168.178.123"});
BENCHMARK_CAPTURE(BM_IPv4Address_Parse, OutOfRange, std::string_view{"192.168.178.256"});
BENCHMARK_CAPTURE(BM_IPv4Address_Parse, Garbage, std::string_view{"not-an-address"});


static void BM_IPv4Address_ToString(benchmark::State &state)
{
	auto address = IPv4Address::fromHostOrder(0xC0A8B27B); // 192.168.178.123

	for (auto _ : state)
	{
		benchmark::DoNotOptimize(address);
		auto text = address.toString();
		benchmark::DoNotOptimize(text);
	}
}
BENCHMARK(BM_IPv4Address_ToString);


static void BM_SocketAddress_ToString(benchmark::State &state)
{
	SocketAddress address{.ip = bench::loopback(), .port = 50123};

	for (auto _ : state)
	{
		benchmark::DoNotOptimize(address);
		auto text = address.toString();
		benchmark::DoNotOptimize(text);
	}
}
BENCHMARK(BM_SocketAddress_ToString);


// Lookup in a map keyed by SocketAddress, as done per datagram by the channel's link table
static void BM_SocketAddress_MapLookup(benchmark::State &state)
{
	const auto					 entries = static_cast<uint16_t>(state.range(0));
	std::map<SocketAddress, int> links;

	for (uint16_t i = 0; i < entries; ++i)
		links.emplace(SocketAddress{.ip = bench::loopback(), .port = static_cast<uint16_t>(50000 + i)}, i);

	uint16_t next = 0;

	for (auto _ : state)
	{
		const SocketAddress key{.ip = bench::loopback(), .port = static_cast<uint16_t>(50000 + next)};
		benchmark::DoNotOptimize(links.find(key));
		next = static_cast<uint16_t>((next + 1) % entries);
	}
}
BENCHMARK(BM_SocketAddress_MapLookup)->ArgName("entries")->Arg(1)->Arg(16)->Arg(256);

} // namespace SocketBenchmarks
