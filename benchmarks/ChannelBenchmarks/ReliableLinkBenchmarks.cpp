/*
  ==============================================================================
	Module:         ReliableLinkBenchmarks
	Description:    The CPU cost of the reliability protocol itself: no sockets,
					no threads, a manual clock
  ==============================================================================
*/

#include <benchmark/benchmark.h>

#include "BenchUtil.h"
#include "LinkWire.h"
#include "NetLinkConstants.h"

using namespace netlink;
using namespace netlink::channel;
using bench::LinkWire;


namespace ChannelBenchmarks
{

// Time: one message from a to b, fragmented, acknowledged and delivered, while loss_pct of the Data packets are lost.
// retransmit_pct: share of the Data packets that had to be sent again. Equal to loss_pct when every lost packet is resent
// exactly once; a higher value means spurious retransmissions.
static void BM_ReliableLink_Transfer(benchmark::State &state)
{
	const auto size	   = static_cast<size_t>(state.range(0));
	const auto lossPct = state.range(1);
	const auto payload = bench::makePayload(size);

	LinkWire   wire;
	const auto drop = LinkWire::dropEveryNth(lossPct > 0 ? 100 / lossPct : 0);

	for (auto _ : state)
	{
		wire.a.queueReliable(ChannelId::Application, 0, payload);

		if (!wire.settle(drop) || wire.takeDeliveredBytesAtB() != size)
		{
			state.SkipWithError("The message was not delivered completely");
			break;
		}
	}

	const auto &stats				 = wire.a.stats();
	state.counters["retransmit_pct"] = bench::percent(stats.retransmissions, stats.dataSent + stats.retransmissions);
	state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * size));
}
BENCHMARK(BM_ReliableLink_Transfer)->ArgNames({"bytes", "loss_pct"})->ArgsProduct({{bench::KiB, 64 * bench::KiB}, {0, 5, 20}})->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_ReliableLink_Transfer)
	->ArgNames({"bytes", "loss_pct"})
	->ArgsProduct({{bench::MiB, static_cast<int64_t>(internal::MaxMessagePayload)}, {0, 5, 20}})
	->Unit(benchmark::kMillisecond);

} // namespace ChannelBenchmarks
