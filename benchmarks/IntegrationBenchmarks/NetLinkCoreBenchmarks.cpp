/*
  ==============================================================================
	Module:         NetLinkCoreBenchmarks
	Description:    End to end through the core: two instances on 127.0.0.1
					discover each other and connect for real, then messages go
					through send() and the callbacks on the event thread
  ==============================================================================
*/

#include <benchmark/benchmark.h>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "BenchUtil.h"
#include "Core/NetLinkCore.h"
#include "Loopback.h"

using namespace netlink;


namespace IntegrationBenchmarks
{

static constexpr uint32_t DataType = 1; // counted by B
static constexpr uint32_t EchoType = 2; // counted and sent back by B


// Two NetLinkCores that discovered each other and are connected. B echoes messages of EchoType from inside its callback.
class ConnectedCores
{
public:
	// A first: its stop says goodbye to B and waits for the acknowledgement
	~ConnectedCores()
	{
		a.reset();
		b.reset();
	}

	// Empty on success, otherwise why the setup failed
	std::string setUp()
	{
		if (!a->start(makeConfig("bench-a"), callbacksForA()) || !b->start(makeConfig("bench-b"), callbacksForB()))
			return "NetLinkCore::start failed";

		// Announcing again makes a core look at its discovery port right away instead of with its next tick
		const auto deadline = bench::Clock::now() + std::chrono::seconds{10};

		while (!discoveredByA.waitFor(1, std::chrono::milliseconds{2}) || !discoveredByB.waitFor(1, std::chrono::milliseconds{2}))
		{
			if (bench::Clock::now() > deadline)
				return "The peers did not discover each other over loopback";

			a->startDiscovery();
			b->startDiscovery();
		}

		a->stopDiscovery();
		b->stopDiscovery();

		return connect() ? std::string{} : "The peers did not connect";
	}

	// A connects to B; true once both report it
	bool connect()
	{
		// Refused for as long as the previous session is still saying goodbye
		const auto deadline = bench::Clock::now() + bench::WaitTimeout;

		while (!a->connect(remoteOfA()))
		{
			if (bench::Clock::now() > deadline)
				return false;

			std::this_thread::yield();
		}

		expectedConnected += 2;
		return connected.waitFor(expectedConnected);
	}

	// A disconnects; true once both report it
	bool disconnect()
	{
		a->disconnect(remoteOfA());
		expectedDisconnected += 2;
		return disconnected.waitFor(expectedDisconnected);
	}

	PeerId remoteOfA()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return remote;
	}

	// Declared before the cores: their event threads report into these until the cores are gone
	std::mutex					 mutex;
	PeerId						 remote;
	bench::CompletionCounter	 discoveredByA;
	bench::CompletionCounter	 discoveredByB;
	bench::CompletionCounter	 connected;
	bench::CompletionCounter	 disconnected;
	bench::CompletionCounter	 receivedAtB;
	bench::CompletionCounter	 repliesAtA;
	uint64_t					 expectedConnected{0};
	uint64_t					 expectedDisconnected{0};

	std::unique_ptr<NetLinkCore> a = std::make_unique<NetLinkCore>(onLoopback());
	std::unique_ptr<NetLinkCore> b = std::make_unique<NetLinkCore>(onLoopback());

private:
	static NetLinkCoreDependencies onLoopback()
	{
		NetLinkCoreDependencies dependencies;
		dependencies.localInterface = [] { return std::optional(LocalInterface{.ip = bench::loopback(), .mask = net::IPv4Address::fromHostOrder(0xFF000000u)}); };
		return dependencies;
	}

	static NetLinkConfig makeConfig(const std::string &name)
	{
		NetLinkConfig config;
		config.displayName	 = name;
		config.appId		 = "netlink-benchmarks";
		config.appVersion	 = "1.4.2";
		config.discoveryPort = bench::DiscoveryPort;
		config.maxSendRate	 = 0; // what the code and the machine can do, not what the default rate allows
		return config;
	}

	NetLinkCallbacks callbacksForA()
	{
		NetLinkCallbacks callbacks;
		callbacks.onPeerDiscovered = [this](const PeerInfo &peer)
		{
			{
				std::lock_guard<std::mutex> lock(mutex);
				remote = peer.id;
			}
			discoveredByA.notify();
		};
		callbacks.onConnected	 = [this](const PeerInfo &) { connected.notify(); };
		callbacks.onDisconnected = [this](PeerId, DisconnectReason) { disconnected.notify(); };
		callbacks.onMessage		 = [this](PeerId, Lane, Message &&) { repliesAtA.notify(); };
		return callbacks;
	}

	NetLinkCallbacks callbacksForB()
	{
		NetLinkCallbacks callbacks;
		callbacks.onPeerDiscovered = [this](const PeerInfo &) { discoveredByB.notify(); };
		callbacks.onConnected	   = [this](const PeerInfo &) { connected.notify(); };
		callbacks.onDisconnected   = [this](PeerId, DisconnectReason) { disconnected.notify(); };
		callbacks.onMessage		   = [this](const PeerId from, Lane, Message &&message)
		{
			if (message.type == EchoType)
				b->send(from, message.type, std::move(message.data), Lane::Reliable, {});

			receivedAtB.notify();
		};
		return callbacks;
	}
};


// Time: a batch of `messages` reliable messages through send() until B's callback received all of them.
// Read items_per_second as messages/s and bytes_per_second as payload throughput.
static void BM_NetLinkCore_Throughput(benchmark::State &state)
{
	const auto	   size		= static_cast<size_t>(state.range(0));
	const auto	   messages = static_cast<uint64_t>(state.range(1));
	const auto	   payload	= bench::makePayload(size);

	ConnectedCores cores;
	if (const auto error = cores.setUp(); !error.empty())
	{
		state.SkipWithError(error);
		return;
	}

	uint64_t expected = 0;

	for (auto _ : state)
	{
		// While the send queue is full send() waits for room, like an application respecting backpressure
		bool sent = true;
		for (uint64_t i = 0; i < messages && sent; ++i)
			sent = cores.a->send(cores.remoteOfA(), DataType, std::vector<uint8_t>(payload), Lane::Reliable, bench::WaitTimeout) == SendResult::Queued;

		expected += messages;

		if (!sent || !cores.receivedAtB.waitFor(expected))
		{
			state.SkipWithError("Not every message arrived");
			break;
		}
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * messages));
	state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * messages * size));
}
BENCHMARK(BM_NetLinkCore_Throughput)
	->ArgNames({"bytes", "messages"})
	->Args({64, 20'000})
	->Args({bench::KiB, 20'000})
	->Args({64 * bench::KiB, 256})
	->Args({bench::MiB, 16})
	->UseRealTime()
	->MeasureProcessCPUTime()
	->Unit(benchmark::kMillisecond);


// Time: A sends, B answers from its callback, until A's callback saw the answer
static void BM_NetLinkCore_RoundTrip(benchmark::State &state)
{
	const auto	   payload = bench::makePayload(static_cast<size_t>(state.range(0)));

	ConnectedCores cores;
	if (const auto error = cores.setUp(); !error.empty())
	{
		state.SkipWithError(error);
		return;
	}

	uint64_t replies = 0;

	for (auto _ : state)
	{
		if (cores.a->send(cores.remoteOfA(), EchoType, std::vector<uint8_t>(payload), Lane::Reliable, {}) != SendResult::Queued || !cores.repliesAtA.waitFor(++replies))
		{
			state.SkipWithError("No reply arrived");
			break;
		}
	}
}
BENCHMARK(BM_NetLinkCore_RoundTrip)
	->ArgName("bytes")
	->Arg(64)
	->Arg(bench::KiB)
	->Arg(64 * bench::KiB)
	->UseRealTime()
	->MeasureProcessCPUTime()
	->Unit(benchmark::kMicrosecond);


// Time: connect() between two discovered peers until both report the session (the disconnect before each round is not timed)
static void BM_NetLinkCore_Connect(benchmark::State &state)
{
	ConnectedCores cores;
	if (const auto error = cores.setUp(); !error.empty())
	{
		state.SkipWithError(error);
		return;
	}

	for (auto _ : state)
	{
		if (!cores.disconnect())
		{
			state.SkipWithError("The peers did not disconnect");
			break;
		}

		const auto start = bench::Clock::now();

		if (!cores.connect())
		{
			state.SkipWithError("The peers did not connect");
			break;
		}

		state.SetIterationTime(bench::secondsSince(start));
	}
}
BENCHMARK(BM_NetLinkCore_Connect)->UseManualTime()->MeasureProcessCPUTime()->Unit(benchmark::kMicrosecond);

} // namespace IntegrationBenchmarks
