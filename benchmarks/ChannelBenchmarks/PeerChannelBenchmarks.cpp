/*
  ==============================================================================
	Module:         PeerChannelBenchmarks
	Description:    Production PeerChannels on real UDP loopback sockets:
					throughput, request/reply latency, many senders into one
					receiver and one sender to many receivers
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
#include "Loopback.h"
#include "NetLinkConstants.h"

using namespace netlink;
using bench::LoopbackPeers;


namespace ChannelBenchmarks
{

static constexpr uint32_t DataType = 1;
static constexpr uint32_t EchoType = 2;


// The hub sends to peer 0. Peer 0 counts every message and answers those of EchoType; the hub counts the answers.
class SenderReceiver
{
public:
	bool open()
	{
		if (!peers.open())
			return false;

		peers.peer(0).setMessageCallback(
			[this](const std::string &sender, const uint32_t type, std::vector<uint8_t> data)
			{
				if (type == EchoType)
					peers.peer(0).sendMessage(sender, EchoType, data, DeliveryMode::ReliableOrdered);

				received.notify();
			});

		peers.hub().setMessageCallback([this](const std::string &, uint32_t, std::vector<uint8_t>) { replies.notify(); });

		peers.start();
		return true;
	}

	bool send(const std::vector<uint8_t> &payload, const uint32_t type) { return bench::sendWithBackpressure(peers.hub(), peers.peerName(0), type, payload); }

	// Declared before the channels: their threads report into these until the channels are gone
	bench::CompletionCounter received;
	bench::CompletionCounter replies;
	LoopbackPeers			 peers{1};
};


// Time: a batch of `messages` reliable messages sent (waiting whenever the send queue is full) until all arrived.
// Read items_per_second as messages/s and bytes_per_second as payload throughput.
static void BM_PeerChannel_Throughput(benchmark::State &state)
{
	const auto	   size		= static_cast<size_t>(state.range(0));
	const auto	   messages = static_cast<uint64_t>(state.range(1));
	const auto	   payload	= bench::makePayload(size);

	SenderReceiver channels;
	if (!channels.open())
	{
		state.SkipWithError("Could not bind the channels to 127.0.0.1");
		return;
	}

	uint64_t expected = 0;

	for (auto _ : state)
	{
		bool sent = true;
		for (uint64_t i = 0; i < messages && sent; ++i)
			sent = channels.send(payload, DataType);

		expected += messages;

		if (!sent || !channels.received.waitFor(expected))
		{
			state.SkipWithError("Not every message arrived");
			break;
		}
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * messages));
	state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * messages * size));
}
BENCHMARK(BM_PeerChannel_Throughput)
	->ArgNames({"bytes", "messages"})
	->Args({64, 20'000})
	->Args({bench::KiB, 20'000})
	->Args({64 * bench::KiB, 256})
	->Args({bench::MiB, 16})
	->Args({static_cast<int64_t>(internal::MaxMessagePayload) - 4, 2}) // the largest message the channel accepts
	->UseRealTime()
	->MeasureProcessCPUTime()
	->Unit(benchmark::kMillisecond);


// Time: one reliable request until its reply arrived back (the receiver answers from its message callback)
static void BM_PeerChannel_RoundTrip(benchmark::State &state)
{
	const auto	   payload = bench::makePayload(static_cast<size_t>(state.range(0)));

	SenderReceiver channels;
	if (!channels.open())
	{
		state.SkipWithError("Could not bind the channels to 127.0.0.1");
		return;
	}

	uint64_t replies = 0;

	for (auto _ : state)
	{
		if (!channels.send(payload, EchoType) || !channels.replies.waitFor(++replies))
		{
			state.SkipWithError("No reply arrived");
			break;
		}
	}
}
BENCHMARK(BM_PeerChannel_RoundTrip)
	->ArgName("bytes")
	->Arg(64)
	->Arg(bench::KiB)
	->Arg(64 * bench::KiB)
	->UseRealTime()
	->MeasureProcessCPUTime()
	->Unit(benchmark::kMicrosecond);


// Time: one reliable message until flush() saw it acknowledged: what a graceful shutdown waits for its goodbye
static void BM_PeerChannel_Flush(benchmark::State &state)
{
	const auto	   payload = bench::makePayload(64);

	SenderReceiver channels;
	if (!channels.open())
	{
		state.SkipWithError("Could not bind the channels to 127.0.0.1");
		return;
	}

	for (auto _ : state)
	{
		if (!channels.send(payload, DataType) || !channels.peers.hub().flush(channels.peers.peerName(0), bench::WaitTimeout))
		{
			state.SkipWithError("The message was not acknowledged");
			break;
		}
	}
}
BENCHMARK(BM_PeerChannel_Flush)->UseRealTime()->MeasureProcessCPUTime()->Unit(benchmark::kMicrosecond);


// Time: N peers each stream 500 messages of 1 KiB to the hub at once, until the last one arrived (or delivery stopped).
// delivered_pct: messages that reached the hub. links_lost: links reset per round after exhausting their retransmissions,
// which discards the messages still queued on them.
static void BM_PeerChannel_FanIn(benchmark::State &state)
{
	constexpr uint64_t	  PerSender = 500;
	const auto			  senders	= static_cast<size_t>(state.range(0));
	const auto			  payload	= bench::makePayload(bench::KiB);

	// Declared before the channels: their threads report into these
	bench::CompletionCounter received;
	std::atomic<uint64_t> linksLost{0};
	std::atomic<int64_t>  lastArrival{0}; // steady clock ticks of the latest message at the hub

	LoopbackPeers		  peers(senders);
	if (!peers.open())
	{
		state.SkipWithError("Could not bind the channels to 127.0.0.1");
		return;
	}

	peers.hub().setMessageCallback(
		[&](const std::string &, uint32_t, std::vector<uint8_t>)
		{
			lastArrival.store(bench::Clock::now().time_since_epoch().count());
			received.notify();
		});

	auto countLoss = [&linksLost](const std::string &, const std::string &) { ++linksLost; };
	peers.hub().setOnPeerLost(countLoss);
	for (size_t s = 0; s < senders; ++s)
		peers.peer(s).setOnPeerLost(countLoss);

	peers.start();

	uint64_t delivered = 0;

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
					for (uint64_t i = 0; i < PerSender; ++i)
						bench::sendWithBackpressure(peers.peer(s), LoopbackPeers::HubName, DataType, payload);
				});
		}

		const uint64_t base	 = received.value();
		const auto	   start = bench::Clock::now();
		go.count_down();

		for (auto &thread : threads)
			thread.join();

		// Done when everything arrived, or when nothing arrived for 5 s: the rest was lost with failed links
		received.waitWhileProgressing(base + senders * PerSender, std::chrono::milliseconds{5000});
		delivered += received.value() - base;

		const auto finished = bench::Clock::time_point(bench::Clock::duration(lastArrival.load()));
		state.SetIterationTime(std::chrono::duration<double>(std::max(finished, start) - start).count());
	}

	const auto sent				    = static_cast<uint64_t>(state.iterations()) * senders * PerSender;
	state.counters["delivered_pct"] = bench::percent(delivered, sent);
	state.counters["links_lost"]	= benchmark::Counter(static_cast<double>(linksLost.load()), benchmark::Counter::kAvgIterations);
	state.SetItemsProcessed(static_cast<int64_t>(delivered));

	peers.stop();
}
BENCHMARK(BM_PeerChannel_FanIn)->ArgName("senders")->Arg(1)->Arg(8)->Arg(32)->Arg(128)->UseManualTime()->MeasureProcessCPUTime()->Unit(benchmark::kMillisecond);


// Time: the hub streams 500 messages of 1 KiB to each of N peers, one message per peer in turn, until the last one
// arrived (or delivery stopped). delivered_pct and links_lost: as for FanIn.
static void BM_PeerChannel_FanOut(benchmark::State &state)
{
	constexpr uint64_t		 PerReceiver = 500;
	const auto				 receivers	 = static_cast<size_t>(state.range(0));
	const auto				 payload	 = bench::makePayload(bench::KiB);

	// Declared before the channels: their threads report into these
	bench::CompletionCounter received;
	std::atomic<uint64_t>	 linksLost{0};
	std::atomic<int64_t>	 lastArrival{0}; // steady clock ticks of the latest message at any peer

	LoopbackPeers			 peers(receivers);
	if (!peers.open())
	{
		state.SkipWithError("Could not bind the channels to 127.0.0.1");
		return;
	}

	auto countLoss = [&linksLost](const std::string &, const std::string &) { ++linksLost; };
	peers.hub().setOnPeerLost(countLoss);

	for (size_t r = 0; r < receivers; ++r)
	{
		peers.peer(r).setMessageCallback(
			[&](const std::string &, uint32_t, std::vector<uint8_t>)
			{
				// Every peer reports from a thread of its own: the latest arrival wins
				const int64_t arrival = bench::Clock::now().time_since_epoch().count();
				int64_t		  latest  = lastArrival.load();
				while (latest < arrival && !lastArrival.compare_exchange_weak(latest, arrival))
				{
				}

				received.notify();
			});
		peers.peer(r).setOnPeerLost(countLoss);
	}

	peers.start();

	uint64_t delivered = 0;

	for (auto _ : state)
	{
		const uint64_t base	 = received.value();
		const auto	   start = bench::Clock::now();

		for (uint64_t i = 0; i < PerReceiver; ++i)
		{
			for (size_t r = 0; r < receivers; ++r)
				bench::sendWithBackpressure(peers.hub(), peers.peerName(r), DataType, payload);
		}

		// Done when everything arrived, or when nothing arrived for 5 s: the rest was lost with failed links
		received.waitWhileProgressing(base + receivers * PerReceiver, std::chrono::milliseconds{5000});
		delivered += received.value() - base;

		const auto finished = bench::Clock::time_point(bench::Clock::duration(lastArrival.load()));
		state.SetIterationTime(std::chrono::duration<double>(std::max(finished, start) - start).count());
	}

	const auto sent					= static_cast<uint64_t>(state.iterations()) * receivers * PerReceiver;
	state.counters["delivered_pct"] = bench::percent(delivered, sent);
	state.counters["links_lost"]	= benchmark::Counter(static_cast<double>(linksLost.load()), benchmark::Counter::kAvgIterations);
	state.SetItemsProcessed(static_cast<int64_t>(delivered));

	peers.stop();
}
BENCHMARK(BM_PeerChannel_FanOut)->ArgName("receivers")->Arg(1)->Arg(8)->Arg(32)->UseManualTime()->MeasureProcessCPUTime()->Unit(benchmark::kMillisecond);

} // namespace ChannelBenchmarks
