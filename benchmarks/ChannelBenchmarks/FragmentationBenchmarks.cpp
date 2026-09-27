/*
  ==============================================================================
	Module:         FragmentationBenchmarks
	Description:    Splitting messages into datagram-sized fragments and
					reassembling them, in order and shuffled
  ==============================================================================
*/

#include <benchmark/benchmark.h>

#include <algorithm>
#include <random>
#include <vector>

#include "BenchUtil.h"
#include "Channel/Fragmentation/FragmentationService.h"
#include "Channel/Reliability/ReliableLink.h"

using namespace netlink;
using namespace netlink::channel;


namespace ChannelBenchmarks
{

// The fragment body size the channel really uses
static size_t maxFragmentBody()
{
	return ReliableLink{}.maxFragmentBody();
}


static void BM_Fragmentation_Split(benchmark::State &state)
{
	const auto	 body		 = bench::makePayload(static_cast<size_t>(state.range(0)));
	const size_t maxFragment = maxFragmentBody();

	for (auto _ : state)
	{
		auto fragments = FragmentationService::split(body, maxFragment);
		benchmark::DoNotOptimize(fragments);
	}

	state.counters["fragments"] = static_cast<double>(FragmentationService::fragmentCount(body.size(), maxFragment));
	state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * body.size()));
}
BENCHMARK(BM_Fragmentation_Split)->Apply(bench::messageSizes);


// Feeds every fragment of one message into accept(); args: message bytes, shuffled arrival order
static void BM_Fragmentation_Reassemble(benchmark::State &state)
{
	struct Arrival
	{
		PacketHeader			 header;
		std::span<const uint8_t> body;
	};

	const auto			 body = bench::makePayload(static_cast<size_t>(state.range(0)));
	constexpr auto		 peer = net::SocketAddress{.ip = bench::loopback(), .port = 50000};

	std::vector<Arrival> arrivals;
	uint64_t			 seq = 1;

	for (const auto &fragment : FragmentationService::split(body, maxFragmentBody()))
	{
		PacketHeader header;
		header.flags	   = PacketFlags::data(ChannelId::Application, true);
		header.srcStreamID = 1;
		header.seq		   = seq++;

		if (fragment.isFragmented())
		{
			header.flags.setFragment(true, fragment.isLast());
			header.fragIndex = fragment.index;
			header.fragCount = fragment.count;
		}

		arrivals.push_back({.header = header, .body = fragment.body});
	}

	if (state.range(1) != 0)
		std::ranges::shuffle(arrivals, std::mt19937{7});

	FragmentationService service;

	for (auto _ : state)
	{
		// A completed message leaves no partial state behind, so the same seqs can be fed again
		for (const auto &[header, fragmentBody] : arrivals)
		{
			auto message = service.accept(peer, header, fragmentBody);
			benchmark::DoNotOptimize(message);
		}
	}

	state.counters["fragments"] = static_cast<double>(arrivals.size());
	state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * body.size()));
}
BENCHMARK(BM_Fragmentation_Reassemble)->ArgNames({"bytes", "shuffled"})->ArgsProduct({{64, 1024, 64 * 1024, 1024 * 1024}, {0, 1}});

} // namespace ChannelBenchmarks
