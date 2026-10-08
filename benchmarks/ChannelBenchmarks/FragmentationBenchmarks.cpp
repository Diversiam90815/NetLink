/*
  ==============================================================================
	Module:         FragmentationBenchmarks
	Description:    Reassembling a fragmented message on the receiving side
  ==============================================================================
*/

#include <benchmark/benchmark.h>

#include <span>
#include <vector>

#include "BenchUtil.h"
#include "Channel/Fragmentation/MessageAssembler.h"
#include "Channel/Reliability/ReliableLink.h"
#include "TransportConstants.h"

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


// The fragments of a message, cut the way a link cuts them
static std::vector<Arrival> arrivalsOf(std::span<const uint8_t> message)
{
	const size_t		 count = fragmentsOf(message.size());
	std::vector<Arrival> arrivals;

	for (size_t index = 0; index < count; ++index)
	{
		const size_t offset = index * MaxFragmentBody;

		PacketHeader header;
		header.flags	   = PacketFlags::data(Lane::Reliable);
		header.srcStreamID = 1;
		header.seq		   = 1 + index;

		if (count > 1)
		{
			header.flags.setFragment(true, index + 1 == count);
			header.fragIndex   = static_cast<uint16_t>(index);
			header.fragCount   = static_cast<uint16_t>(count);
			header.totalLength = static_cast<uint32_t>(message.size());
		}

		arrivals.push_back({.header = header, .body = message.subspan(offset, std::min(MaxFragmentBody, message.size() - offset))});
	}

	return arrivals;
}


// Time: all fragments of one message fed into the assembler, in the order their stream delivers them, until the whole
// message comes out. Reordering is not a case here: the link's reorder buffer sorts that out before.
static void BM_Fragmentation_Reassemble(benchmark::State &state)
{
	const auto		 message  = bench::makePayload(static_cast<size_t>(state.range(0)));
	const auto		 arrivals = arrivalsOf(message);

	MessageAssembler assembler;

	for (auto _ : state)
	{
		for (const auto &[header, body] : arrivals)
		{
			auto reassembled = assembler.accept(header, body);
			benchmark::DoNotOptimize(reassembled);
		}
	}

	state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * message.size()));
}
BENCHMARK(BM_Fragmentation_Reassemble)->ArgName("bytes")->Arg(64 * bench::KiB)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_Fragmentation_Reassemble)->ArgName("bytes")->Arg(bench::MiB)->Arg(static_cast<int64_t>(internal::MaxMessagePayload))->Unit(benchmark::kMillisecond);

} // namespace ChannelBenchmarks
