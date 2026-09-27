/*
  ==============================================================================
	Module:         ReliableLinkBenchmarks
	Description:    The reliability protocol itself without sockets or threads.
					Load: 16 MiB messages under loss, 100k queued messages.
  ==============================================================================
*/

#include <benchmark/benchmark.h>

#include <climits>
#include <vector>

#include "BenchUtil.h"
#include "LinkWire.h"
#include "NetLinkConstants.h"

using namespace netlink;
using namespace netlink::channel;
using bench::LinkWire;


namespace ChannelBenchmarks
{

// Moves batches of messages a -> b until all are acknowledged; args: message bytes, drop every n-th Data packet (0 = lossless)
static void BM_ReliableLink_Transfer(benchmark::State &state)
{
	const auto	 size	 = static_cast<size_t>(state.range(0));
	const size_t batch	 = bench::batchFor(size);
	const auto	 payload = bench::makePayload(size);

	LinkWire	 wire;
	const auto	 drop = LinkWire::dropEveryNth(state.range(1));

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
BENCHMARK(BM_ReliableLink_Transfer)->ArgNames({"bytes", "dropEvery"})->ArgsProduct({{64, bench::KiB, 64 * bench::KiB, bench::MiB}, {0, 20, 5}});
BENCHMARK(BM_ReliableLink_Transfer)
	->Name("BM_ReliableLink_Load_Transfer")
	->ArgNames({"bytes", "dropEvery"})
	->ArgsProduct({{4 * bench::MiB, static_cast<int64_t>(internal::MaxMessagePayload)}, {0, 20, 5}})
	->Unit(benchmark::kMillisecond);


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


// 10k / 100k small messages queued at once behind a send queue sized for them (NetLinkConfig::sendQueueCapacity)
static void BM_ReliableLink_Load_DeepQueue(benchmark::State &state)
{
	const auto		  messages = static_cast<size_t>(state.range(0));
	const auto		  payload  = bench::makePayload(64);

	ReliabilityConfig config;
	config.sendQueueCapacity = messages;

	LinkWire wire(config);

	for (auto _ : state)
	{
		for (size_t i = 0; i < messages; ++i)
			wire.a.queueReliable(ChannelId::Application, payload, wire.now);

		if (!wire.settle() || wire.takeDeliveredBytesAtB() != messages * payload.size())
		{
			state.SkipWithError("Not every message was delivered");
			break;
		}
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * messages));
}
BENCHMARK(BM_ReliableLink_Load_DeepQueue)->ArgName("messages")->Arg(10'000)->Arg(100'000)->Unit(benchmark::kMillisecond);

} // namespace ChannelBenchmarks
