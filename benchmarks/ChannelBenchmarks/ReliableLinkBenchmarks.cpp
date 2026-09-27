/*
  ==============================================================================
	Module:         ReliableLinkBenchmarks
	Description:    The reliability protocol itself without sockets or threads.
  ==============================================================================
*/

#include <benchmark/benchmark.h>

#include <climits>
#include <vector>

#include "BenchUtil.h"
#include "LinkWire.h"

using namespace netlink;
using namespace netlink::channel;
using bench::LinkWire;


namespace ChannelBenchmarks
{

// Loses every n-th Data packet on the way (retransmissions included), deterministic
static LinkWire::Drop dropEveryNth(const int64_t n)
{
	if (n <= 0)
		return {};

	return [n, count = int64_t{0}](const PacketHeader &header) mutable { return header.flags.kind() == PacketKind::Data && ++count % n == 0; };
}


// Moves batches of messages a -> b until all are acknowledged; args: message bytes, drop every n-th Data packet (0 = lossless)
static void BM_ReliableLink_Transfer(benchmark::State &state)
{
	const auto	 size	 = static_cast<size_t>(state.range(0));
	const size_t batch	 = bench::batchFor(size);
	const auto	 payload = bench::makePayload(size);

	LinkWire	 wire;
	const auto	 drop = dropEveryNth(state.range(1));

	for (auto _ : state)
	{
		for (size_t i = 0; i < batch; ++i)
			wire.a.queueReliable(ChannelId::Application, payload, wire.now);

		if (!wire.settle(drop))
		{
			state.SkipWithError("The links did not settle");
			break;
		}

		if (wire.takeDeliveredBytesAtB() != batch * size)
		{
			state.SkipWithError("Not every byte was delivered");
			break;
		}
	}

	const auto &stats			  = wire.a.stats();
	state.counters["retransmits"] = benchmark::Counter(static_cast<double>(stats.retransmissions), benchmark::Counter::kAvgIterations);
	state.counters["datagrams"]	  = benchmark::Counter(static_cast<double>(stats.dataSent + stats.retransmissions), benchmark::Counter::kAvgIterations);
	state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * batch));
	state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * batch * size));
}
BENCHMARK(BM_ReliableLink_Transfer)->ArgNames({"bytes", "dropEvery"})->ArgsProduct({{64, 1024, 64 * 1024, 1024 * 1024}, {0, 20, 5}});


// A full window of unacknowledged packets comes due: onTimer() retransmits all of them
static void BM_ReliableLink_RetransmitWindow(benchmark::State &state)
{
	ReliabilityConfig config;
	config.maxRetransmits = INT_MAX; // keep retransmitting, never fail the link

	LinkWire   wire(config);
	const auto payload = bench::makePayload(1024);

	for (size_t i = 0; i < WindowSize; ++i)
		wire.a.queueReliable(ChannelId::Application, payload, wire.now);

	static_cast<void>(wire.a.takeOutgoing()); // everything is lost

	for (auto _ : state)
	{
		wire.now += config.maxRto; // backoff is capped at maxRto: every packet is due again
		wire.a.onTimer(wire.now);

		auto datagrams = wire.a.takeOutgoing();
		benchmark::DoNotOptimize(datagrams);
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * WindowSize));
}
BENCHMARK(BM_ReliableLink_RetransmitWindow);


// queueReliable() with a full window and a full send queue: the backpressure path
static void BM_ReliableLink_QueueFull(benchmark::State &state)
{
	ReliabilityConfig config;
	config.sendQueueOverflow = state.range(0) == 0 ? OverflowPolicy::DropNewest : OverflowPolicy::DropOldest;

	LinkWire   wire(config);
	const auto payload = bench::makePayload(64);

	for (size_t i = 0; i < WindowSize + config.sendQueueCapacity; ++i)
		wire.a.queueReliable(ChannelId::Application, payload, wire.now);

	static_cast<void>(wire.a.takeOutgoing());

	for (auto _ : state)
		benchmark::DoNotOptimize(wire.a.queueReliable(ChannelId::Application, payload, wire.now));

	state.SetLabel(config.sendQueueOverflow == OverflowPolicy::DropNewest ? "DropNewest" : "DropOldest");
}
BENCHMARK(BM_ReliableLink_QueueFull)->ArgName("dropOldest")->Arg(0)->Arg(1);


// One unreliable datagram: build, stamp and encode
static void BM_ReliableLink_SendUnreliable(benchmark::State &state)
{
	LinkWire   wire;
	const auto payload = bench::makePayload(static_cast<size_t>(state.range(0)));

	for (auto _ : state)
	{
		wire.a.sendUnreliable(ChannelId::Application, payload);
		auto datagrams = wire.a.takeOutgoing();
		benchmark::DoNotOptimize(datagrams);
	}

	state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * state.range(0));
}
BENCHMARK(BM_ReliableLink_SendUnreliable)->ArgName("bytes")->Arg(64)->Arg(512)->Arg(1100);

} // namespace ChannelBenchmarks
