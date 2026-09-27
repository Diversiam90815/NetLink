/*
  ==============================================================================
	Module:         ConnectionServiceBenchmarks
	Description:    The connection lifecycle between two peers.
					Load: session setup on a saturated channel, many peers
					inviting one hub at once.
  ==============================================================================
*/

#include <benchmark/benchmark.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "BenchUtil.h"
#include "LoopbackChannelPair.h"
#include "LoopbackSwarm.h"
#include "ServiceWiring.h"
#include "Traffic.h"
#include "ConnectionService/ConnectionService.h"
#include "ConnectionService/ReadySyncTracker.h"

using namespace netlink;
using bench::LoopbackChannelPair;
using bench::LoopbackSwarm;


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
	// Wired as NetLinkCore wires it; the remote counts as validated, as after a successful validation
	void wire(PeerChannel &channel, ConnectionService &service, const int remotePort, const char *remoteName)
	{
		service.setLocalIP(bench::loopback());
		service.setCallbacks(bench::connectionStatusHandler(channel, [this](const ConnectionStatusUpdate &update) { count(update); }));
		bench::wireConnection(channel, service);
		service.onPeerValidated(bench::validatedRemote(remoteName, remotePort));
	}

	void count(const ConnectionStatusUpdate &update)
	{
		switch (update.type)
		{
		case ConnectionStatusUpdate::Type::Established: established.notify(); break;
		case ConnectionStatusUpdate::Type::Closed: closed.notify(); break;
		case ConnectionStatusUpdate::Type::Failed:
		case ConnectionStatusUpdate::Type::Declined: failed.store(true); break;
		default: break;
		}
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
// traffic: a streams 1 KiB messages to b the whole time; control signals then compete with a full send window.
BENCHMARK_DEFINE_F(BM_ConnectionService, Establish)(benchmark::State &state)
{
	uint64_t						 expectedEstablished = 0;
	uint64_t						 expectedClosed		 = 0;

	std::optional<bench::BackgroundStream> traffic;
	if (state.range(0) != 0 && pair->a.getBoundPort() != 0)
		traffic.emplace(pair->a, LoopbackChannelPair::NameB, 1024);

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

	if (traffic)
		state.counters["stream_msgs"] = static_cast<double>(traffic->sent());

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK_REGISTER_F(BM_ConnectionService, Establish)->ArgName("traffic")->Arg(0)->UseManualTime();
BENCHMARK_REGISTER_F(BM_ConnectionService, Establish)
	->Name("BM_ConnectionService_Load_EstablishUnderTraffic")
	->ArgName("traffic")
	->Arg(1)
	->UseManualTime()
	->Unit(benchmark::kMillisecond);


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


// N validated peers invite one hub at the same moment. The hub accepts one and declines the rest (it is busy).
// Timed until every peer got its answer; the winning session is closed again between rounds, untimed.
static void BM_ConnectionService_Load_InvitationStorm(benchmark::State &state)
{
	using Type											= ConnectionStatusUpdate::Type;
	const auto										peerCount = static_cast<size_t>(state.range(0));

	// Declared first: the threads of everything below report into these
	bench::CompletionCounter						answered; // peer side: established, declined or failed
	bench::CompletionCounter						hubEstablished;
	bench::CompletionCounter						closed;
	std::atomic<uint64_t>							sessions{0};
	std::mutex										winnerMutex;
	std::optional<size_t>							winner;

	LoopbackSwarm									swarm(peerCount);
	std::unique_ptr<ConnectionService>				hubService;
	std::vector<std::unique_ptr<ConnectionService>> peerServices;

	if (!swarm.open())
	{
		state.SkipWithError("Could not bind the swarm to 127.0.0.1");
		return;
	}

	hubService = std::make_unique<ConnectionService>(swarm.hub);
	hubService->setConfig(ConnectionConfig{.autoAcceptConnection = true});
	hubService->setCallbacks(bench::connectionStatusHandler(swarm.hub,
															[&](const ConnectionStatusUpdate &update)
															{
																if (update.type == Type::Established)
																	hubEstablished.notify();
																else if (update.type == Type::Closed)
																	closed.notify();
															}));
	bench::wireConnection(swarm.hub, *hubService);

	for (size_t i = 0; i < peerCount; ++i)
	{
		auto &channel = *swarm.peers[i];
		auto &service = *peerServices.emplace_back(std::make_unique<ConnectionService>(channel));

		service.setCallbacks(bench::connectionStatusHandler(channel,
															[&, i](const ConnectionStatusUpdate &update)
															{
																switch (update.type)
																{
																case Type::Established:
																{
																	std::lock_guard<std::mutex> lock(winnerMutex);
																	winner = i;
																	++sessions;
																	answered.notify();
																	break;
																}
																case Type::Declined:
																case Type::Failed: answered.notify(); break;
																case Type::Closed: closed.notify(); break;
																default: break;
																}
															}));
		bench::wireConnection(channel, service);

		service.onPeerValidated(bench::validatedRemote(LoopbackSwarm::HubName, swarm.hub.getBoundPort()));
		hubService->onPeerValidated(bench::validatedRemote(swarm.names[i], channel.getBoundPort()));
	}

	swarm.start();

	uint64_t expectedAnswers = 0;
	uint64_t expectedClosed	 = 0;
	uint64_t rounds			 = 0;

	for (auto _ : state)
	{
		const auto start = bench::Clock::now();

		for (const auto &service : peerServices)
			service->initiateConnection(LoopbackSwarm::HubName);

		expectedAnswers += peerCount;
		const bool complete = answered.waitFor(expectedAnswers, bench::LoadTimeout) && hubEstablished.waitFor(++rounds, bench::LoadTimeout);
		state.SetIterationTime(bench::secondsSince(start));

		std::optional<size_t> won;
		{
			std::lock_guard<std::mutex> lock(winnerMutex);
			won = std::exchange(winner, std::nullopt);
		}

		if (!complete || !won)
		{
			state.SkipWithError("Not every invitation was answered, or no session was established");
			break;
		}

		// Untimed: the winner closes its session, both sides report it closed
		expectedClosed += 2;
		if (!peerServices[*won]->closeConnection(LoopbackSwarm::HubName) || !closed.waitFor(expectedClosed, bench::LoadTimeout))
		{
			state.SkipWithError("The winning session did not close");
			break;
		}
	}

	state.counters["sessions"] = benchmark::Counter(static_cast<double>(sessions.load()), benchmark::Counter::kAvgIterations);
	state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * peerCount));

	// Channel threads call into the services and the services into the channels: stop the threads first
	swarm.stop();
	peerServices.clear();
	hubService.reset();
}
BENCHMARK(BM_ConnectionService_Load_InvitationStorm)->ArgName("peers")->Arg(8)->Arg(32)->Arg(128)->UseManualTime()->Unit(benchmark::kMillisecond);

} // namespace ConnectionBenchmarks
