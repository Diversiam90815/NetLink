/*
  ==============================================================================
	Module:         BoundedQueueBenchmarks
	Description:    Ring buffer behind the send queues of every ReliableLink
  ==============================================================================
*/

#include <benchmark/benchmark.h>

#include <cstdint>
#include <optional>

#include "BenchUtil.h"
#include "Channel/Queue/BoundedQueue.h"
#include "Channel/Reliability/ReliableLink.h"

using namespace netlink;
using namespace netlink::channel;


namespace QueueBenchmarks
{

// Items: a plain integer, and the message type the link queues (movable, owns its body)
template <typename T>
static T makeItem(size_t index);

template <>
uint64_t makeItem<uint64_t>(const size_t index)
{
	return index;
}

template <>
OutboundMessage makeItem<OutboundMessage>(const size_t index)
{
	return OutboundMessage{.channel = ChannelId::Application, .body = bench::makePayload(256, static_cast<uint32_t>(index))};
}


template <typename T>
static BoundedQueue<T> halfFilledQueue(const size_t capacity, const OverflowPolicy policy = OverflowPolicy::DropNewest)
{
	BoundedQueue<T> queue(capacity, policy);

	for (size_t i = 0; i < capacity / 2; ++i)
		queue.push(makeItem<T>(i));

	return queue;
}


// Steady state: one push and one pop per iteration. Items are recycled, so no allocation is measured.
template <typename T>
static void BM_BoundedQueue_PushPop(benchmark::State &state)
{
	auto queue = halfFilledQueue<T>(static_cast<size_t>(state.range(0)));
	T	 spare = makeItem<T>(0);

	for (auto _ : state)
	{
		queue.push(std::move(spare));
		spare = std::move(*queue.pop());
		benchmark::DoNotOptimize(spare);
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK_TEMPLATE(BM_BoundedQueue_PushPop, uint64_t)->ArgName("capacity")->Arg(16)->Arg(1024)->Arg(65536);
BENCHMARK_TEMPLATE(BM_BoundedQueue_PushPop, OutboundMessage)->ArgName("capacity")->Arg(16)->Arg(1024)->Arg(65536);


// Burst: fill the queue completely, then drain it
template <typename T>
static void BM_BoundedQueue_FillDrain(benchmark::State &state)
{
	const auto		capacity = static_cast<size_t>(state.range(0));
	BoundedQueue<T> queue(capacity);
	std::vector<T>	items;

	for (size_t i = 0; i < capacity; ++i)
		items.push_back(makeItem<T>(i));

	for (auto _ : state)
	{
		for (auto &item : items)
			queue.push(std::move(item));

		for (auto &item : items)
			item = std::move(*queue.pop());

		benchmark::ClobberMemory();
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * capacity * 2));
}
BENCHMARK_TEMPLATE(BM_BoundedQueue_FillDrain, uint64_t)->ArgName("capacity")->Arg(16)->Arg(1024);
BENCHMARK_TEMPLATE(BM_BoundedQueue_FillDrain, OutboundMessage)->ArgName("capacity")->Arg(16)->Arg(1024);


// Push into a full queue: DropNewest refuses the item, DropOldest evicts the oldest one and hands it back
template <typename T>
static void BM_BoundedQueue_Overflow(benchmark::State &state)
{
	const auto		 policy	  = state.range(0) == 0 ? OverflowPolicy::DropNewest : OverflowPolicy::DropOldest;
	constexpr size_t capacity = 1024;
	BoundedQueue<T>	 queue(capacity, policy);

	for (size_t i = 0; i < capacity; ++i)
		queue.push(makeItem<T>(i));

	T				 spare = makeItem<T>(capacity);
	std::optional<T> evicted;

	for (auto _ : state)
	{
		auto result = queue.push(std::move(spare), &evicted);
		benchmark::DoNotOptimize(result);

		if (evicted)
			spare = std::move(*evicted);
	}

	state.SetLabel(policy == OverflowPolicy::DropNewest ? "DropNewest" : "DropOldest");
	state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK_TEMPLATE(BM_BoundedQueue_Overflow, uint64_t)->ArgName("dropOldest")->Arg(0)->Arg(1);
BENCHMARK_TEMPLATE(BM_BoundedQueue_Overflow, OutboundMessage)->ArgName("dropOldest")->Arg(0)->Arg(1);

} // namespace QueueBenchmarks
