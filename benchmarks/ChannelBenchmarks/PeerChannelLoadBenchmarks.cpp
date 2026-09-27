/*
  ==============================================================================
	Module:         PeerChannelLoadBenchmarks
	Description:    PeerChannel under heavy load on real UDP loopback sockets
  ==============================================================================
*/

#include <benchmark/benchmark.h>

#include <algorithm>
#include <atomic>
#include <latch>
#include <string>
#include <thread>
#include <vector>

#include "BenchUtil.h"
#include "LoopbackChannelPair.h"
#include "LoopbackSwarm.h"
#include "NetLinkConstants.h"
#include "Traffic.h"

using namespace netlink;
using bench::LoopbackChannelPair;
using bench::LoopbackSwarm;


namespace ChannelBenchmarks
{

static constexpr uint32_t DataType	   = 1;
static constexpr uint32_t SentinelType = 2;

// Largest application message: the channel's message limit minus the 4-byte type prefix
static constexpr int64_t  MaxMessage   = static_cast<int64_t>(internal::MaxMessagePayload) - 4;


// A loopback pair whose receiver b counts every application message; the sentinel type is counted separately
struct CountingPair
{
	explicit CountingPair(const PeerChannelConfig &config = {}) : channels(config) {}

	bool open()
	{
		if (!channels.open())
			return false;

		channels.b.setMessageCallback(
			[this](const std::string &, const uint32_t type, std::vector<uint8_t>)
			{
				if (type == SentinelType)
					sentinels.notify();
				else
					received.notify();
			});

		channels.start();
		return true;
	}

	bool send(std::span<const uint8_t> payload, const DeliveryMode mode = DeliveryMode::ReliableOrdered)
	{
		return channels.a.sendMessage(LoopbackChannelPair::NameB, DataType, payload, mode);
	}

	// Declared before the channels: their threads report into these until the channels are gone
	bench::CompletionCounter received;
	bench::CompletionCounter sentinels;
	LoopbackChannelPair		 channels;
};


// A long stream of reliable messages, up to the largest message the channel accepts, from an application that waits
// whenever the send queue is full.
// blocked_ms: time per stream the sender was held back by backpressure.
static void BM_PeerChannel_Load_Stream(benchmark::State &state)
{
	const auto	 size	  = static_cast<size_t>(state.range(0));
	const auto	 messages = static_cast<uint64_t>(state.range(1));
	const auto	 payload  = bench::makePayload(size);

	CountingPair pair;
	if (!pair.open())
	{
		state.SkipWithError("Could not bind the peer channels to 127.0.0.1");
		return;
	}

	uint64_t delivered = 0;
	double	 blockedMs = 0.0;

	for (auto _ : state)
	{
		bool ok = true;

		for (uint64_t i = 0; i < messages && ok; ++i)
		{
			const auto blocked = bench::sendWithBackpressure(pair.channels.a, LoopbackChannelPair::NameB, DataType, payload);
			ok				   = blocked.has_value();
			if (ok)
				blockedMs += std::chrono::duration<double, std::milli>(*blocked).count();
		}

		delivered += messages;

		if (!ok || !pair.received.waitFor(delivered, bench::LoadTimeout))
		{
			state.SkipWithError("The stream did not get through");
			break;
		}
	}

	state.counters["blocked_ms"] = benchmark::Counter(blockedMs, benchmark::Counter::kAvgIterations);
	state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * messages));
	state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * messages * size));
}
BENCHMARK(BM_PeerChannel_Load_Stream)
	->ArgNames({"bytes", "messages"})
	->Args({64, 100'000})
	->Args({bench::KiB, 100'000})
	->Args({64 * bench::KiB, 256})
	->Args({bench::MiB, 16})
	->Args({4 * bench::MiB, 4})
	->Args({MaxMessage, 2})
	->UseRealTime()
	->Unit(benchmark::kMillisecond);


// A burst far beyond the send queue (1024 messages), sent without waiting. Timed until the burst is drained.
// accepted: share send() took; delivered: share that reached b. DropNewest refuses the excess, DropOldest evicts unsent messages.
static void BM_PeerChannel_Load_Burst(benchmark::State &state)
{
	const auto		  messages	 = static_cast<uint64_t>(state.range(0));
	const bool		  dropOldest = state.range(1) != 0;
	const auto		  payload	 = bench::makePayload(256);
	const auto		  sentinel	 = bench::makePayload(1);

	PeerChannelConfig config;
	config.reliability.sendQueueOverflow = dropOldest ? OverflowPolicy::DropOldest : OverflowPolicy::DropNewest;

	CountingPair pair(config);
	if (!pair.open())
	{
		state.SkipWithError("Could not bind the peer channels to 127.0.0.1");
		return;
	}

	uint64_t accepted  = 0;
	uint64_t sentinels = 0;

	for (auto _ : state)
	{
		for (uint64_t i = 0; i < messages; ++i)
		{
			if (pair.send(payload))
				++accepted;
		}

		// Reliable delivery is ordered: once the sentinel arrived, everything that survived the burst did
		if (!bench::sendWithBackpressure(pair.channels.a, LoopbackChannelPair::NameB, SentinelType, sentinel) || !pair.sentinels.waitFor(++sentinels, bench::LoadTimeout))
		{
			state.SkipWithError("The burst was not drained");
			break;
		}
	}

	const double sent			= static_cast<double>(state.iterations() * messages);
	state.counters["accepted"]	= static_cast<double>(accepted) / std::max(sent, 1.0);
	state.counters["delivered"] = static_cast<double>(pair.received.value()) / std::max(sent, 1.0);
	state.SetLabel(dropOldest ? "DropOldest" : "DropNewest");
	state.SetItemsProcessed(static_cast<int64_t>(pair.received.value()));
}
BENCHMARK(BM_PeerChannel_Load_Burst)->ArgNames({"messages", "dropOldest"})->ArgsProduct({{10'000, 100'000}, {0, 1}})->UseRealTime()->Unit(benchmark::kMillisecond);


// Unreliable messages as fast as the sender can go. Timed is sending; arrived: share that reached b.
static void BM_PeerChannel_Load_UnreliableFlood(benchmark::State &state)
{
	const auto	 messages = static_cast<uint64_t>(state.range(0));
	const auto	 payload  = bench::makePayload(static_cast<size_t>(state.range(1)));

	CountingPair pair;
	if (!pair.open())
	{
		state.SkipWithError("Could not bind the peer channels to 127.0.0.1");
		return;
	}

	uint64_t sent = 0;

	for (auto _ : state)
	{
		for (uint64_t i = 0; i < messages; ++i)
		{
			if (pair.send(payload, DeliveryMode::UnreliableSequenced))
				++sent;
		}
	}

	pair.received.waitFor(sent, std::chrono::milliseconds{500}); // untimed grace for datagrams still on their way

	state.counters["arrived"] = static_cast<double>(pair.received.value()) / static_cast<double>(std::max<uint64_t>(sent, 1));
	state.SetItemsProcessed(static_cast<int64_t>(sent));
	state.SetBytesProcessed(static_cast<int64_t>(sent) * state.range(1));
}
BENCHMARK(BM_PeerChannel_Load_UnreliableFlood)->ArgNames({"messages", "bytes"})->Args({100'000, 64})->Args({100'000, 1176})->UseRealTime()->Unit(benchmark::kMillisecond);


// Many peers stream to one hub at the same time, each from its own thread, each respecting backpressure.
// Timed until the last message reached the hub. Under this load the OS drops datagrams at the hub's socket; links that
// exhaust their retransmissions are reset and their unsent messages are gone (links_lost). delivered: share that arrived.
static void BM_PeerChannel_Load_FanIn(benchmark::State &state)
{
	const auto				 senders   = static_cast<size_t>(state.range(0));
	const auto				 perSender = static_cast<uint64_t>(state.range(1));
	const auto				 payload   = bench::makePayload(1024);

	// Declared before the swarm: its threads report into these
	bench::CompletionCounter received;
	std::atomic<uint64_t>	 linksLost{0};
	std::atomic<int64_t>	 lastArrival{0}; // steady clock ticks of the latest message at the hub

	LoopbackSwarm			 swarm(senders);

	if (!swarm.open())
	{
		state.SkipWithError("Could not bind the swarm to 127.0.0.1");
		return;
	}

	swarm.hub.setMessageCallback(
		[&](const std::string &, uint32_t, std::vector<uint8_t>)
		{
			lastArrival.store(bench::Clock::now().time_since_epoch().count());
			received.notify();
		});

	swarm.hub.setOnPeerLost([&linksLost](const std::string &, const std::string &) { ++linksLost; });
	for (const auto &peer : swarm.peers)
		peer->setOnPeerLost([&linksLost](const std::string &, const std::string &) { ++linksLost; });

	swarm.start();

	uint64_t sent	   = 0;
	uint64_t delivered = 0;
	bool	 complete  = true;

	for (auto _ : state)
	{
		std::latch				 go(1);
		std::vector<std::thread> threads;

		for (size_t s = 0; s < senders; ++s)
		{
			threads.emplace_back(
				[&, s]
				{
					go.wait();
					for (uint64_t i = 0; i < perSender; ++i)
						static_cast<void>(bench::sendWithBackpressure(*swarm.peers[s], LoopbackSwarm::HubName, DataType, payload));
				});
		}

		const uint64_t base	 = received.value();
		const auto	   start = bench::Clock::now();
		go.count_down();

		for (auto &thread : threads)
			thread.join();

		sent += senders * perSender;

		// Done when everything arrived, or when nothing arrived for 5 s (the rest was lost with failed links)
		complete &= received.waitWhileProgressing(base + senders * perSender, std::chrono::milliseconds{5000});
		delivered += received.value() - base;

		const auto finished = bench::Clock::time_point(bench::Clock::duration(lastArrival.load()));
		state.SetIterationTime(std::chrono::duration<double>(std::max(finished, start) - start).count());
	}

	state.counters["delivered"]	 = static_cast<double>(delivered) / static_cast<double>(std::max<uint64_t>(sent, 1));
	state.counters["links_lost"] = static_cast<double>(linksLost.load());
	state.SetLabel(complete ? "complete" : "INCOMPLETE: messages lost with failed links");
	state.SetItemsProcessed(static_cast<int64_t>(delivered));
	state.SetBytesProcessed(static_cast<int64_t>(delivered * payload.size()));

	swarm.stop();
}
BENCHMARK(BM_PeerChannel_Load_FanIn)->ArgNames({"senders", "perSender"})->ArgsProduct({{8, 32, 128}, {500}})->UseManualTime()->Unit(benchmark::kMillisecond);


// Several application threads send over the same channel to the same peer (lock contention on the send path)
static void BM_PeerChannel_Load_ConcurrentSenders(benchmark::State &state)
{
	const auto	 threadCount = static_cast<size_t>(state.range(0));
	const auto	 perThread	 = static_cast<uint64_t>(20'000 / threadCount);
	const auto	 payload	 = bench::makePayload(256);

	CountingPair pair;
	if (!pair.open())
	{
		state.SkipWithError("Could not bind the peer channels to 127.0.0.1");
		return;
	}

	uint64_t expected = 0;

	for (auto _ : state)
	{
		std::atomic<bool>		 failed{false};
		std::latch				 go(1);
		std::vector<std::thread> threads;

		for (size_t t = 0; t < threadCount; ++t)
		{
			threads.emplace_back(
				[&]
				{
					go.wait();
					for (uint64_t i = 0; i < perThread; ++i)
					{
						if (!bench::sendWithBackpressure(pair.channels.a, LoopbackChannelPair::NameB, DataType, payload))
						{
							failed.store(true);
							return;
						}
					}
				});
		}

		const auto start = bench::Clock::now();
		go.count_down();

		expected += threadCount * perThread;
		const bool complete = pair.received.waitFor(expected, bench::LoadTimeout);
		state.SetIterationTime(bench::secondsSince(start));

		for (auto &thread : threads)
			thread.join();

		if (!complete || failed.load())
		{
			state.SkipWithError("Not every message arrived");
			break;
		}
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * threadCount * perThread));
}
BENCHMARK(BM_PeerChannel_Load_ConcurrentSenders)->ArgName("threads")->Arg(1)->Arg(2)->Arg(4)->Arg(8)->UseManualTime()->Unit(benchmark::kMillisecond);


} // namespace ChannelBenchmarks
