/*
  ==============================================================================
	Module:         FragmentationBenchmarks
	Description:    Reassembling a fragmented message on the receiving side
  ==============================================================================
*/

#include <benchmark/benchmark.h>

#include <algorithm>
#include <random>
#include <span>
#include <vector>

#include "BenchUtil.h"
#include "Channel/Fragmentation/FragmentationService.h"
#include "Channel/Reliability/ReliableLink.h"
#include "NetLinkConstants.h"

using namespace netlink;
using namespace netlink::channel;


namespace ChannelBenchmarks
{

// One received fragment: its header as the link delivers it, its body pointing into the original message
struct Arrival
{
	PacketHeader			 header;
	std::span<const uint8_t> body;
};


// The fragments of a message, cut with the fragment size the channel really uses
static std::vector<Arrival> arrivalsOf(std::span<const uint8_t> message)
{
	std::vector<Arrival> arrivals;
	uint64_t			 seq = 1;

	for (const auto &fragment : FragmentationService::split(message, ReliableLink{}.maxFragmentBody()))
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

	return arrivals;
}


// Time: all fragments of one message fed into accept() until the whole message comes out, in order or shuffled
static void BM_Fragmentation_Reassemble(benchmark::State &state)
{
	const auto	   message	= bench::makePayload(static_cast<size_t>(state.range(0)));
	constexpr auto peer		= net::SocketAddress{.ip = bench::loopback(), .port = 50000};
	auto		   arrivals = arrivalsOf(message);

	if (state.range(1) != 0)
		std::ranges::shuffle(arrivals, std::mt19937{7});

	FragmentationService service;

	for (auto _ : state)
	{
		// A completed message leaves no partial state behind, so the same seqs can be fed again
		for (const auto &[header, body] : arrivals)
		{
			auto reassembled = service.accept(peer, header, body);
			benchmark::DoNotOptimize(reassembled);
		}
	}

	state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * message.size()));
}
BENCHMARK(BM_Fragmentation_Reassemble)->ArgNames({"bytes", "shuffled"})->ArgsProduct({{64 * bench::KiB}, {0, 1}})->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_Fragmentation_Reassemble)
	->ArgNames({"bytes", "shuffled"})
	->ArgsProduct({{bench::MiB, static_cast<int64_t>(internal::MaxMessagePayload)}, {0, 1}})
	->Unit(benchmark::kMillisecond);

} // namespace ChannelBenchmarks
