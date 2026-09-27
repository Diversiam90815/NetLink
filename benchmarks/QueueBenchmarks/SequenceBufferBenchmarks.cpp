/*
  ==============================================================================
	Module:         SequenceBufferBenchmarks
	Description:    Seq-indexed store behind the in-flight, reorder and ack
					windows of every ReliableLink
  ==============================================================================
*/

#include <benchmark/benchmark.h>

#include <cstdint>
#include <vector>

#include "BenchUtil.h"
#include "Channel/Queue/SequenceBuffer.h"
#include "Channel/Reliability/ReliabilityConfig.h"

using namespace netlink::channel;


namespace QueueBenchmarks
{

// Shaped like the link's window entries: a few scalars plus an owned datagram body
struct WindowEntry
{
	uint64_t			 deadline{0};
	int					 transmissions{0};
	std::vector<uint8_t> body;
};

using Window = SequenceBuffer<WindowEntry, WindowSize>;


static void fill(Window &window, const uint64_t first, const size_t count)
{
	for (uint64_t seq = first; seq < first + count; ++seq)
		window.insert(seq, WindowEntry{.deadline = seq, .transmissions = 1, .body = bench::makePayload(1176)});
}


// The sender's pattern: the oldest seq gets acknowledged (take), a new one goes in flight (insert)
static void BM_SequenceBuffer_Slide(benchmark::State &state)
{
	const auto inFlight = static_cast<uint64_t>(state.range(0));
	Window	   window;
	fill(window, 1, inFlight);

	uint64_t base = 1;

	for (auto _ : state)
	{
		auto acknowledged = window.take(base);
		window.insert(base + inFlight, std::move(*acknowledged));
		++base;
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_SequenceBuffer_Slide)->ArgName("inFlight")->Arg(16)->Arg(128)->Arg(static_cast<int64_t>(WindowSize));


static void BM_SequenceBuffer_Find(benchmark::State &state)
{
	const bool hit = state.range(0) != 0;
	Window	   window;
	fill(window, 1, WindowSize);

	uint64_t seq = hit ? 1 : WindowSize + 1; // a miss lands on an occupied slot with another seq

	for (auto _ : state)
	{
		benchmark::DoNotOptimize(window.find(seq));
		seq = hit ? (seq % WindowSize) + 1 : seq + 1;
	}

	state.SetLabel(hit ? "hit" : "miss");
}
BENCHMARK(BM_SequenceBuffer_Find)->ArgName("hit")->Arg(1)->Arg(0);


// The retransmission timer visits every slot of the window
static void BM_SequenceBuffer_ForEach(benchmark::State &state)
{
	Window window;
	fill(window, 1, static_cast<size_t>(state.range(0)));

	for (auto _ : state)
	{
		window.forEach(
			[](uint64_t, WindowEntry &entry)
			{
				++entry.deadline;
				return true;
			});
		benchmark::ClobberMemory();
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(WindowSize));
}
BENCHMARK(BM_SequenceBuffer_ForEach)->ArgName("occupied")->Arg(8)->Arg(64)->Arg(static_cast<int64_t>(WindowSize));

} // namespace QueueBenchmarks
