/*
  ==============================================================================
	Module:         FragmentationBenchmarks
	Description:    Splitting messages into datagram-sized fragments and
					reassembling them, in order and shuffled.
					Load: messages up to 16 MiB, reassembly from 255 peers at once.
  ==============================================================================
*/

#include <benchmark/benchmark.h>

#include <algorithm>
#include <random>
#include <vector>

#include "BenchUtil.h"
#include "Channel/Fragmentation/FragmentationService.h"
#include "Channel/Reliability/ReliableLink.h"
#include "NetLinkConstants.h"

using namespace netlink;
using namespace netlink::channel;


namespace ChannelBenchmarks
{

// The fragment body size the channel really uses
static size_t maxFragmentBody()
{
	return ReliableLink{}.maxFragmentBody();
}


static constexpr int64_t MaxMessage = static_cast<int64_t>(internal::MaxMessagePayload);


// One received fragment: its header as the link delivers it, its body pointing into the original message
struct Arrival
{
	PacketHeader			 header;
	std::span<const uint8_t> body;
};


// The fragments of a message in the order the link delivers them
static std::vector<Arrival> arrivalsOf(std::span<const uint8_t> message)
{
	std::vector<Arrival> arrivals;
	uint64_t			 seq = 1;

	for (const auto &fragment : FragmentationService::split(message, maxFragmentBody()))
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
BENCHMARK(BM_Fragmentation_Split)->Name("BM_Fragmentation_Load_Split")->ArgName("bytes")->Arg(4 * bench::MiB)->Arg(MaxMessage)->Unit(benchmark::kMicrosecond);


// Feeds every fragment of one message into accept(); args: message bytes, shuffled arrival order
static void BM_Fragmentation_Reassemble(benchmark::State &state)
{
	const auto	   body		= bench::makePayload(static_cast<size_t>(state.range(0)));
	constexpr auto peer		= net::SocketAddress{.ip = bench::loopback(), .port = 50000};
	auto		   arrivals = arrivalsOf(body);

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
BENCHMARK(BM_Fragmentation_Reassemble)->ArgNames({"bytes", "shuffled"})->ArgsProduct({{64, bench::KiB, 64 * bench::KiB, bench::MiB}, {0, 1}});
BENCHMARK(BM_Fragmentation_Reassemble)
	->Name("BM_Fragmentation_Load_Reassemble")
	->ArgNames({"bytes", "shuffled"})
	->ArgsProduct({{4 * bench::MiB, MaxMessage}, {0, 1}})
	->Unit(benchmark::kMillisecond);


// Many peers each deliver a 256 KiB message at the same time; their fragments arrive interleaved
static void BM_Fragmentation_Load_ManyPeers(benchmark::State &state)
{
	const auto						peers	 = static_cast<size_t>(state.range(0));
	const auto						message	 = bench::makePayload(256 * bench::KiB);
	const auto						arrivals = arrivalsOf(message);

	std::vector<net::SocketAddress> addresses;
	for (size_t p = 0; p < peers; ++p)
		addresses.push_back({.ip = bench::loopback(), .port = static_cast<uint16_t>(40000 + p)});

	FragmentationService service;

	for (auto _ : state)
	{
		// Round robin: fragment i of every peer, then fragment i + 1 of every peer, ...
		for (const auto &[header, body] : arrivals)
		{
			for (const auto &address : addresses)
			{
				auto reassembled = service.accept(address, header, body);
				benchmark::DoNotOptimize(reassembled);
			}
		}
	}

	state.counters["fragments"] = static_cast<double>(arrivals.size() * peers);
	state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * peers));
	state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * peers * message.size()));
}
BENCHMARK(BM_Fragmentation_Load_ManyPeers)->ArgName("peers")->Arg(16)->Arg(64)->Arg(255)->Unit(benchmark::kMillisecond);

} // namespace ChannelBenchmarks
