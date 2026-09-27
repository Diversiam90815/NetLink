/*
  ==============================================================================
	Module:         ConnectionServiceBenchmarks
	Description:    The connection lifecycle between two peerss
  ==============================================================================
*/

#include <benchmark/benchmark.h>

#include <atomic>
#include <memory>
#include <string>

#include "BenchUtil.h"
#include "LoopbackChannelPair.h"
#include "ConnectionService/ConnectionService.h"
#include "ConnectionService/ReadySyncTracker.h"

using namespace netlink;
using bench::LoopbackChannelPair;


namespace ConnectionBenchmarks
{

class BM_ConnectionService : public benchmark::Fixture
{
public:
	using benchmark::Fixture::SetUp;
	using benchmark::Fixture::TearDown;

	void SetUp(benchmark::State &state) override
	{
		established.reset();
		closed.reset();
		failed.store(false);

		pair = std::make_unique<LoopbackChannelPair>();

		if (!pair->open())
		{
			state.SkipWithError("Could not bind the peer channels to 127.0.0.1");
			return;
		}

		serviceA = std::make_unique<ConnectionService>(pair->a);
		serviceB = std::make_unique<ConnectionService>(pair->b);

		// b accepts every invitation, like an application answering right away
		serviceB->setConfig(ConnectionConfig{.autoAcceptConnection = true});

		wire(pair->a, *serviceA, pair->b.getBoundPort(), LoopbackChannelPair::NameB);
		wire(pair->b, *serviceB, pair->a.getBoundPort(), LoopbackChannelPair::NameA);

		pair->start();
	}

	void TearDown(benchmark::State &) override
	{
		// Channel threads call into the services, services into the channels: stop the threads first
		pair->a.stop();
		pair->b.stop();
		serviceA.reset();
		serviceB.reset();
		pair.reset();
	}

protected:
	// Same wiring as NetLinkCore::wireServices(); the remote counts as validated, as after a successful validation
	void wire(PeerChannel &channel, ConnectionService &service, const int remotePort, const char *remoteName)
	{
		service.setLocalIP(bench::loopback());

		ChannelConnectionCallbacks signals;
		signals.onConnectRequested		 = [&service](const std::string &name) { service.onReceivedInvitation(name); };
		signals.onConnectRequestAnswered = [&service](const std::string &name, const bool accepted, const std::string &reason)
		{ service.onReceivedAnswerToInvite(name, accepted, reason); };
		signals.onDisconnectReceived = [&service](const std::string &name) { service.onDisconnectReceived(name); };
		signals.onReadyFlagReceived	 = [&service](const std::string &name) { service.onReadyFlagReceived(name); };
		channel.setConnectionCallbacks(std::move(signals));
		channel.setOnPeerLost([&service](const std::string &name, const std::string &reason) { service.onPeerLost(name, reason); });

		// NetLinkCore::onConnectionStatus: keepalive for the session, session traffic ends with it
		ConnectionServiceCallbacks callbacks;
		callbacks.onStatusUpdate = [this, &channel](const ConnectionStatusUpdate &update)
		{
			switch (update.type)
			{
			case ConnectionStatusUpdate::Type::Established:
				channel.setKeepAlive(update.endpoint.displayName, true);
				established.notify();
				break;

			case ConnectionStatusUpdate::Type::Closed:
				channel.setKeepAlive(update.endpoint.displayName, false);
				channel.dropApplicationTraffic(update.endpoint.displayName);
				closed.notify();
				break;

			case ConnectionStatusUpdate::Type::Failed:
			case ConnectionStatusUpdate::Type::Declined: failed.store(true); break;

			default: break;
			}
		};
		service.setCallbacks(std::move(callbacks));

		ValidationResult validated;
		validated.remoteEndpoint = DiscoveryEndpoint{.IPAddress = bench::loopback(), .port = remotePort, .displayName = remoteName};
		validated.status		 = ValidationResult::Status::ReadyToConnect;
		validated.canConnect	 = true;
		service.onPeerValidated(validated);
	}

	// a invites b; true once both sides report the established session
	bool establish(uint64_t &expectedEstablished)
	{
		if (!serviceA->initiateConnection(LoopbackChannelPair::NameB))
			return false;

		expectedEstablished += 2;
		return established.waitFor(expectedEstablished) && !failed.load();
	}

	// a closes; true once both sides report the closed session
	bool close(uint64_t &expectedClosed)
	{
		if (!serviceA->closeConnection(LoopbackChannelPair::NameB))
			return false;

		expectedClosed += 2;
		return closed.waitFor(expectedClosed);
	}

	bench::CompletionCounter			 established;
	bench::CompletionCounter			 closed;
	std::atomic<bool>					 failed{false};

	std::unique_ptr<ConnectionService>	 serviceA;
	std::unique_ptr<ConnectionService>	 serviceB;
	std::unique_ptr<LoopbackChannelPair> pair;
};


// Invitation -> auto-accept -> ready flags -> both Established. The teardown between rounds is not timed.
BENCHMARK_DEFINE_F(BM_ConnectionService, Establish)(benchmark::State &state)
{
	uint64_t expectedEstablished = 0;
	uint64_t expectedClosed		 = 0;

	for (auto _ : state)
	{
		const auto start = bench::Clock::now();

		if (!establish(expectedEstablished))
		{
			state.SkipWithError("The connection was not established");
			return;
		}

		state.SetIterationTime(bench::secondsSince(start));

		if (!close(expectedClosed))
		{
			state.SkipWithError("The connection was not closed");
			return;
		}
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK_REGISTER_F(BM_ConnectionService, Establish)->UseManualTime();


// A full session lifecycle: establish, then close until both sides report it closed
BENCHMARK_DEFINE_F(BM_ConnectionService, EstablishAndClose)(benchmark::State &state)
{
	uint64_t expectedEstablished = 0;
	uint64_t expectedClosed		 = 0;

	for (auto _ : state)
	{
		if (!establish(expectedEstablished) || !close(expectedClosed))
		{
			state.SkipWithError("The session lifecycle did not complete");
			return;
		}
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK_REGISTER_F(BM_ConnectionService, EstablishAndClose)->UseRealTime();


// The session's ready-flag bookkeeping on its own
static void BM_ReadySyncTracker_BothReady(benchmark::State &state)
{
	ReadySyncTracker tracker;

	for (auto _ : state)
	{
		tracker.setLocalReady();
		tracker.setRemoteReady();
		benchmark::DoNotOptimize(tracker.bothReady());
		tracker.reset();
	}
}
BENCHMARK(BM_ReadySyncTracker_BothReady);

} // namespace ConnectionBenchmarks
