/*
  ==============================================================================
	Module:         PacketCodecBenchmarks
	Description:    Wire encoding and decoding of every datagram on the channel
  ==============================================================================
*/

#include <benchmark/benchmark.h>

#include <vector>

#include "BenchUtil.h"
#include "Channel/Protocol/PacketHeader.h"

using namespace netlink::channel;


namespace ChannelBenchmarks
{

static PacketHeader makeHeader(const bool fragmented)
{
	PacketHeader header;
	header.flags	   = PacketFlags::data(ChannelId::Application, true);
	header.srcStreamID = 0x1234ABCD;
	header.dstStreamID = 0x0BADF00D;
	header.seq		   = 4711;

	if (fragmented)
	{
		header.flags.setFragment(true, false);
		header.fragIndex = 3;
		header.fragCount = 10;
	}

	return header;
}


// Args: body bytes, fragmented header
static void codecArgs(benchmark::internal::Benchmark *b)
{
	b->ArgNames({"bytes", "fragmented"});
	for (const int64_t size : {0, 64, 512, 1176})
	{
		b->Args({size, 0});
		b->Args({size, 1});
	}
}


static void BM_PacketCodec_Encode(benchmark::State &state)
{
	const auto header = makeHeader(state.range(1) != 0);
	const auto body	  = bench::makePayload(static_cast<size_t>(state.range(0)));

	for (auto _ : state)
	{
		auto datagram = encodePacket(header, body);
		benchmark::DoNotOptimize(datagram);
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
	state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * (header.encodedSize() + body.size())));
}
BENCHMARK(BM_PacketCodec_Encode)->Apply(codecArgs);


static void BM_PacketCodec_Decode(benchmark::State &state)
{
	const auto datagram = encodePacket(makeHeader(state.range(1) != 0), bench::makePayload(static_cast<size_t>(state.range(0))));

	for (auto _ : state)
	{
		auto packet = decodePacket(datagram);
		benchmark::DoNotOptimize(packet);
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
	state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * datagram.size()));
}
BENCHMARK(BM_PacketCodec_Decode)->Apply(codecArgs);


// Foreign traffic on the channel port is rejected at the magic check
static void BM_PacketCodec_DecodeForeign(benchmark::State &state)
{
	const auto datagram = bench::makePayload(512);

	for (auto _ : state)
	{
		auto packet = decodePacket(datagram);
		benchmark::DoNotOptimize(packet);
	}
}
BENCHMARK(BM_PacketCodec_DecodeForeign);

} // namespace ChannelBenchmarks
