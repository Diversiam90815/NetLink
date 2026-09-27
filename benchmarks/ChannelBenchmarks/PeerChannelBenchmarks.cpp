/*
  ==============================================================================
	Module:         PeerChannelBenchmarks
	Description:    Two production PeerChannels on real UDP loopback sockets
  ==============================================================================
*/

#include <benchmark/benchmark.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "BenchUtil.h"
#include "LoopbackChannelPair.h"

using namespace netlink;
using bench::LoopbackChannelPair;


namespace ChannelBenchmarks
{

class BM_PeerChannel : public benchmark::Fixture
{
public:
	static constexpr uint32_t DataType = 1; // counted by b
	static constexpr uint32_t EchoType = 2; // counted and sent back by b

	using benchmark::Fixture::SetUp;
	using benchmark::Fixture::TearDown;

	void SetUp(benchmark::State &state) override
	{
		pair = std::make_unique<LoopbackChannelPair>();

		if (!pair->open())
		{
			state.SkipWithError("Could not bind the peer channels to 127.0.0.1");
			return;
		}

		receivedAtB.reset();
		repliesAtA.reset();
		signalsAtA.reset();

		pair->b.setMessageCallback(
			[this](const std::string &sender, const uint32_t type, std::vector<uint8_t> data)
			{
				if (type == EchoType)
					pair->b.sendMessage(sender, EchoType, data, DeliveryMode::ReliableOrdered);

				receivedAtB.notify();
			});

		pair->a.setMessageCallback([this](const std::string &, uint32_t, std::vector<uint8_t>) { repliesAtA.notify(); });

		// b answers every ready flag with its own
		ChannelConnectionCallbacks atB;
		atB.onReadyFlagReceived = [this](const std::string &sender) { pair->b.sendReadyFlag(sender); };
		pair->b.setConnectionCallbacks(atB);

		ChannelConnectionCallbacks atA;
		atA.onReadyFlagReceived = [this](const std::string &) { signalsAtA.notify(); };
		pair->a.setConnectionCallbacks(atA);

		pair->start();
	}

	void TearDown(benchmark::State &) override { pair.reset(); }

protected:
	bool send(const std::vector<uint8_t> &payload, const uint32_t type, const DeliveryMode mode) const
	{
		return pair->a.sendMessage(LoopbackChannelPair::NameB, type, payload, mode);
	}

	// Declared before the pair: its I/O threads report into them until the pair is gone
	bench::CompletionCounter			 receivedAtB;
	bench::CompletionCounter			 repliesAtA;
	bench::CompletionCounter			 signalsAtA;

	std::unique_ptr<LoopbackChannelPair> pair;
};


// Reliable, ordered messages a -> b; one iteration is a batch of about 1 MiB, timed until b received all of it
BENCHMARK_DEFINE_F(BM_PeerChannel, Reliable)(benchmark::State &state)
{
	const auto	 size	  = static_cast<size_t>(state.range(0));
	const size_t batch	  = bench::batchFor(size);
	const auto	 payload  = bench::makePayload(size);
	uint64_t	 expected = 0;

	for (auto _ : state)
	{
		for (size_t i = 0; i < batch; ++i)
		{
			if (!send(payload, DataType, DeliveryMode::ReliableOrdered))
			{
				state.SkipWithError("sendMessage refused a reliable message");
				return;
			}
		}

		expected += batch;

		if (!receivedAtB.waitFor(expected))
		{
			state.SkipWithError("Not every reliable message arrived");
			return;
		}
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * batch));
	state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * batch * size));
}
BENCHMARK_REGISTER_F(BM_PeerChannel, Reliable)->Apply(bench::messageSizes)->UseRealTime();


// Unreliable messages: timed is the send path. arrived: share that reached b (loopback may drop under load).
BENCHMARK_DEFINE_F(BM_PeerChannel, Unreliable)(benchmark::State &state)
{
	const auto payload = bench::makePayload(static_cast<size_t>(state.range(0)));
	uint64_t   refused = 0;

	for (auto _ : state)
	{
		if (!send(payload, DataType, DeliveryMode::UnreliableSequenced))
			++refused;
	}

	const auto sent = static_cast<uint64_t>(state.iterations()) - refused;
	receivedAtB.waitFor(sent, std::chrono::milliseconds{200}); // untimed grace period for datagrams still on their way

	state.counters["refused"] = static_cast<double>(refused);
	state.counters["arrived"] = static_cast<double>(receivedAtB.value()) / static_cast<double>(std::max<uint64_t>(sent, 1));
	state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
	state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * state.range(0));
}
BENCHMARK_REGISTER_F(BM_PeerChannel, Unreliable)->ArgName("bytes")->Arg(64)->Arg(512)->Arg(1176)->UseRealTime();


// Request/reply: a sends reliably, b answers from its message callback, timed until the reply is back at a
BENCHMARK_DEFINE_F(BM_PeerChannel, RoundTrip)(benchmark::State &state)
{
	const auto payload = bench::makePayload(static_cast<size_t>(state.range(0)));
	uint64_t   replies = 0;

	for (auto _ : state)
	{
		if (!send(payload, EchoType, DeliveryMode::ReliableOrdered) || !repliesAtA.waitFor(++replies))
		{
			state.SkipWithError("No reply arrived");
			return;
		}
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK_REGISTER_F(BM_PeerChannel, RoundTrip)->ArgName("bytes")->Arg(64)->Arg(1024)->Arg(64 * 1024)->UseRealTime();


// A control signal (JSON on the control channel) and its answer
BENCHMARK_DEFINE_F(BM_PeerChannel, SignalRoundTrip)(benchmark::State &state)
{
	uint64_t answers = 0;

	for (auto _ : state)
	{
		if (!pair->a.sendReadyFlag(LoopbackChannelPair::NameB) || !signalsAtA.waitFor(++answers))
		{
			state.SkipWithError("No answer to the ready flag arrived");
			return;
		}
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK_REGISTER_F(BM_PeerChannel, SignalRoundTrip)->UseRealTime();


// Queue a batch of 1 KiB messages and wait in flush() until all were acknowledged
BENCHMARK_DEFINE_F(BM_PeerChannel, Flush)(benchmark::State &state)
{
	const auto payload = bench::makePayload(1024);
	const auto batch   = static_cast<size_t>(state.range(0));

	for (auto _ : state)
	{
		for (size_t i = 0; i < batch; ++i)
			send(payload, DataType, DeliveryMode::ReliableOrdered);

		if (!pair->a.flush(LoopbackChannelPair::NameB, bench::CompletionTimeout))
		{
			state.SkipWithError("flush() timed out");
			return;
		}
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * batch));
}
BENCHMARK_REGISTER_F(BM_PeerChannel, Flush)->ArgName("batch")->Arg(1)->Arg(64)->UseRealTime();

} // namespace ChannelBenchmarks
