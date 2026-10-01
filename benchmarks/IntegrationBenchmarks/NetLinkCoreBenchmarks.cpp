/*
  ==============================================================================
	Module:         NetLinkCoreBenchmarks
	Description:    End to end through the composition root with its default
					dependencies: discovery, validation and connection happen for
					real, then messages go through the public send() and callbacks.
					Both peers run on this host: A on 127.0.0.1, B on 127.0.0.2.
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
#include "Socket/UdpSocket.h"

using namespace netlink;


namespace IntegrationBenchmarks
{

// Kept off NetLink's default port, so an application running on this machine is not disturbed
static constexpr int	  DiscoveryPort = 45455;

static constexpr uint32_t DataType		= 1; // counted by B
static constexpr uint32_t EchoType		= 2; // counted and sent back by B


// Two NetLinkCores that discovered, validated and connected to each other. B accepts every invitation and echoes
// messages of EchoType, both from inside its callbacks.
class ConnectedCores
{
public:
	// A first: its shutdown says goodbye to B and waits for the acknowledgement
	~ConnectedCores()
	{
		a.reset();
		b.reset();
	}

	// Empty on success, otherwise why the setup failed
	std::string setUp()
	{
		if (!net::UdpSocket::bind({.ip = bench::loopback(2), .port = 0}))
			return "127.0.0.2 is not usable on this host (macOS: sudo ifconfig lo0 alias 127.0.0.2)";

		a->configure(makeConfig("bench-a"), callbacksForA());
		b->configure(makeConfig("bench-b"), callbacksForB());

		if (!a->init() || !b->init())
			return "NetLinkCore::init failed";

		a->setLocalAddress(bench::loopback(1).toString());
		b->setLocalAddress(bench::loopback(2).toString());

		if (!a->startDiscovery() || !b->startDiscovery())
			return "Discovery could not be started";

		// Discovery announces every 2 s: leave room for a lost first announcement
		if (!discoveredByA.waitFor(1, std::chrono::seconds{10}) || !discoveredByB.waitFor(1, std::chrono::seconds{10}))
			return "The peers did not discover each other over loopback";

		return connect() ? std::string{} : "The peers did not connect";
	}

	// A connects to B; true once both report Connected
	bool connect()
	{
		std::optional<Endpoint> remote;
		{
			std::lock_guard<std::mutex> lock(mutex);
			remote = remoteOfA;
		}

		expectedConnected += 2;
		return remote && a->connectTo(*remote) && connected.waitFor(expectedConnected);
	}

	// A disconnects; true once both report Disconnected
	bool disconnect()
	{
		a->disconnect();
		expectedDisconnected += 2;
		return disconnected.waitFor(expectedDisconnected);
	}

	// Declared before the cores: their event threads report into these until the cores are gone
	std::mutex					 mutex;
	std::optional<Endpoint>		 remoteOfA;
	bench::CompletionCounter	 discoveredByA;
	bench::CompletionCounter	 discoveredByB;
	bench::CompletionCounter	 connected;
	bench::CompletionCounter	 disconnected;
	bench::CompletionCounter	 receivedAtB;
	bench::CompletionCounter	 repliesAtA;
	uint64_t					 expectedConnected{0};
	uint64_t					 expectedDisconnected{0};

	std::unique_ptr<NetLinkCore> a = std::make_unique<NetLinkCore>();
	std::unique_ptr<NetLinkCore> b = std::make_unique<NetLinkCore>();

private:
	static NetLinkConfig makeConfig(const std::string &name)
	{
		NetLinkConfig config;
		config.localDisplayName	  = name;
		config.discoveryPort	  = DiscoveryPort;
		config.secret			  = "a-shared-application-secret";
		config.applicationVersion = "1.4.2";
		return config;
	}

	NetLinkCallbacks callbacksForA()
	{
		NetLinkCallbacks callbacks;
		callbacks.onRemoteDiscovered = [this](const Endpoint &remote)
		{
			{
				std::lock_guard<std::mutex> lock(mutex);
				remoteOfA = remote;
			}
			discoveredByA.notify();
		};
		callbacks.onConnectionChanged = [this](const ConnectionEvent &event) { countConnectionEvent(event); };
		callbacks.onMessageReceived	  = [this](const Message &) { repliesAtA.notify(); };
		return callbacks;
	}

	NetLinkCallbacks callbacksForB()
	{
		NetLinkCallbacks callbacks;
		callbacks.onRemoteDiscovered  = [this](const Endpoint &) { discoveredByB.notify(); };
		callbacks.onConnectionChanged = [this](const ConnectionEvent &event)
		{
			if (event.state == ConnectionState::PendingInbound)
				b->respondToConnection(true);

			countConnectionEvent(event);
		};
		callbacks.onMessageReceived = [this](const Message &message)
		{
			if (message.type == EchoType)
				b->send(message.type, message.data, DeliveryMode::ReliableOrdered);

			receivedAtB.notify();
		};
		return callbacks;
	}

	void countConnectionEvent(const ConnectionEvent &event)
	{
		if (event.state == ConnectionState::Connected)
			connected.notify();
		else if (event.state == ConnectionState::Disconnected)
			disconnected.notify();
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
		for (uint64_t i = 0; i < messages; ++i)
		{
			// send() refuses while the send queue is full: wait like an application respecting backpressure
			const auto deadline = bench::Clock::now() + bench::WaitTimeout;
			while (!cores.a->send(DataType, payload, DeliveryMode::ReliableOrdered) && bench::Clock::now() < deadline)
				std::this_thread::yield();
		}

		expected += messages;

		if (!cores.receivedAtB.waitFor(expected))
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
		if (!cores.a->send(EchoType, payload, DeliveryMode::ReliableOrdered) || !cores.repliesAtA.waitFor(++replies))
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


// Time: connectTo() between two validated peers until both report Connected (the disconnect before each round is not timed)
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
