/*
  ==============================================================================
	Module:         NetLinkCoreBenchmarks
	Description:    End to end through the composition root with its default dependencies
					Both peers run on this host: A on 127.0.0.1, B on 127.0.0.2
  ==============================================================================
*/

#include <benchmark/benchmark.h>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "BenchUtil.h"
#include "Core/NetLinkCore.h"
#include "Socket/UdpSocket.h"

using namespace netlink;


namespace IntegrationBenchmarks
{

// Kept off NetLink's default port, so an application running on this machine is not disturbed
static constexpr int	  DiscoveryPort	   = 45455;

// Discovery announces every 2 s: leave room for a lost first announcement
static constexpr auto	  DiscoveryTimeout = std::chrono::milliseconds{10000};

static constexpr uint32_t DataType		   = 1; // counted by B
static constexpr uint32_t EchoType		   = 2; // counted and sent back by B


class BM_NetLinkCore : public benchmark::Fixture
{
public:
	using benchmark::Fixture::SetUp;
	using benchmark::Fixture::TearDown;

	void SetUp(benchmark::State &state) override
	{
		resetCounters();

		if (!net::UdpSocket::bind({.ip = bench::loopback(2), .port = 0}))
		{
			state.SkipWithError("127.0.0.2 is not usable on this host (macOS: sudo ifconfig lo0 alias 127.0.0.2)");
			return;
		}

		peerA = std::make_unique<NetLinkCore>();
		peerB = std::make_unique<NetLinkCore>();

		peerA->configure(makeConfig("bench-a"), callbacksForA());
		peerB->configure(makeConfig("bench-b"), callbacksForB());

		if (!peerA->init() || !peerB->init())
		{
			state.SkipWithError("NetLinkCore::init failed");
			return;
		}

		peerA->setLocalAddress(bench::loopback(1).toString());
		peerB->setLocalAddress(bench::loopback(2).toString());

		if (!peerA->startDiscovery() || !peerB->startDiscovery())
		{
			state.SkipWithError("Discovery could not be started");
			return;
		}

		if (!discoveredByA.waitFor(1, DiscoveryTimeout) || !discoveredByB.waitFor(1, DiscoveryTimeout))
		{
			state.SkipWithError("The peers did not discover each other over loopback within 10 s");
			return;
		}

		if (!connect())
			state.SkipWithError("The peers did not connect");
	}

	void TearDown(benchmark::State &) override
	{
		peerA.reset();
		peerB.reset();
	}

protected:
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

	// B accepts every invitation and echoes messages of EchoType, both from inside its callbacks
	NetLinkCallbacks callbacksForB()
	{
		NetLinkCallbacks callbacks;
		callbacks.onRemoteDiscovered  = [this](const Endpoint &) { discoveredByB.notify(); };
		callbacks.onConnectionChanged = [this](const ConnectionEvent &event)
		{
			if (event.state == ConnectionState::PendingInbound)
				peerB->respondToConnection(true);

			countConnectionEvent(event);
		};
		callbacks.onMessageReceived = [this](const Message &message)
		{
			if (message.type == EchoType)
				peerB->send(message.type, message.data, DeliveryMode::ReliableOrdered);

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

	// A connects to B; true once both report Connected
	bool connect()
	{
		std::optional<Endpoint> remote;
		{
			std::lock_guard<std::mutex> lock(mutex);
			remote = remoteOfA;
		}

		if (!remote || !peerA->connectTo(*remote))
			return false;

		expectedConnected += 2;
		return connected.waitFor(expectedConnected);
	}

	// A disconnects; true once both report Disconnected
	bool disconnect()
	{
		peerA->disconnect();
		expectedDisconnected += 2;
		return disconnected.waitFor(expectedDisconnected);
	}

	void resetCounters()
	{
		discoveredByA.reset();
		discoveredByB.reset();
		connected.reset();
		disconnected.reset();
		receivedAtB.reset();
		repliesAtA.reset();
		expectedConnected	 = 0;
		expectedDisconnected = 0;
		remoteOfA.reset();
	}

	// Declared before the peers: their event threads report into these until the peers are gone
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

	std::unique_ptr<NetLinkCore> peerA;
	std::unique_ptr<NetLinkCore> peerB;
};


// Reliable messages A -> B through send() and onMessageReceived; batches of about 1 MiB
BENCHMARK_DEFINE_F(BM_NetLinkCore, Send)(benchmark::State &state)
{
	const auto	 size	  = static_cast<size_t>(state.range(0));
	const size_t batch	  = bench::batchFor(size);
	const auto	 payload  = bench::makePayload(size);
	uint64_t	 expected = 0;

	for (auto _ : state)
	{
		for (size_t i = 0; i < batch; ++i)
		{
			if (!peerA->send(DataType, payload, DeliveryMode::ReliableOrdered))
			{
				state.SkipWithError("send() refused a message");
				return;
			}
		}

		expected += batch;

		if (!receivedAtB.waitFor(expected))
		{
			state.SkipWithError("Not every message arrived");
			return;
		}
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * batch));
	state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * batch * size));
}
BENCHMARK_REGISTER_F(BM_NetLinkCore, Send)->Apply(bench::messageSizes)->UseRealTime();


// A sends, B answers from its callback, timed until A's callback saw the answer
BENCHMARK_DEFINE_F(BM_NetLinkCore, RoundTrip)(benchmark::State &state)
{
	const auto payload = bench::makePayload(static_cast<size_t>(state.range(0)));
	uint64_t   replies = 0;

	for (auto _ : state)
	{
		if (!peerA->send(EchoType, payload, DeliveryMode::ReliableOrdered) || !repliesAtA.waitFor(++replies))
		{
			state.SkipWithError("No reply arrived");
			return;
		}
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK_REGISTER_F(BM_NetLinkCore, RoundTrip)->ArgName("bytes")->Arg(64)->Arg(1024)->Arg(64 * 1024)->UseRealTime();


// Connecting two validated peers: connectTo() until both report Connected. Disconnecting before each round is not timed.
BENCHMARK_DEFINE_F(BM_NetLinkCore, Connect)(benchmark::State &state)
{
	for (auto _ : state)
	{
		if (!disconnect())
		{
			state.SkipWithError("The peers did not disconnect");
			return;
		}

		const auto start = bench::Clock::now();

		if (!connect())
		{
			state.SkipWithError("The peers did not connect");
			return;
		}

		state.SetIterationTime(bench::secondsSince(start));
	}
}
BENCHMARK_REGISTER_F(BM_NetLinkCore, Connect)->UseManualTime();

} // namespace IntegrationBenchmarks
