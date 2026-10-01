/*
  ==============================================================================
	Module:         ConnectionServiceBenchmarks
	Description:    Session setup (invitation, acceptance, ready flags) between
					production ConnectionServices on production PeerChannels
					over real UDP, wired like NetLinkCore
  ==============================================================================
*/

#include <benchmark/benchmark.h>

#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "BenchUtil.h"
#include "Loopback.h"
#include "ServiceWiring.h"
#include "ConnectionService/ConnectionService.h"

using namespace netlink;
using bench::LoopbackPeers;


namespace ConnectionBenchmarks
{

// `peerCount` validated peers invite the hub at the same moment; the hub accepts the first and declines the rest (it is
// busy). Each round is timed until every peer got its answer and the accepted session is established on both sides,
// then the session is closed again (untimed). saturated: the first peer streams to the hub the whole time.
static void inviteHub(benchmark::State &state, const size_t peerCount, const bool saturated)
{
	using Type = ConnectionStatusUpdate::Type;

	// Declared first: the threads of everything below report into these
	bench::CompletionCounter						answered; // peer side: established, declined or failed
	bench::CompletionCounter						hubEstablished;
	bench::CompletionCounter						closed;
	std::mutex										winnerMutex;
	std::vector<size_t>								winners;

	LoopbackPeers									peers(peerCount);
	std::unique_ptr<ConnectionService>				hubService;
	std::vector<std::unique_ptr<ConnectionService>> peerServices;

	if (!peers.open())
	{
		state.SkipWithError("Could not bind the channels to 127.0.0.1");
		return;
	}

	hubService = std::make_unique<ConnectionService>(peers.hub());
	hubService->setConfig(ConnectionConfig{.autoAcceptConnection = true});
	hubService->setCallbacks(bench::connectionStatusHandler(peers.hub(),
															[&](const ConnectionStatusUpdate &update)
															{
																if (update.type == Type::Established)
																	hubEstablished.notify();
																else if (update.type == Type::Closed)
																	closed.notify();
															}));
	bench::wireConnection(peers.hub(), *hubService);

	for (size_t i = 0; i < peerCount; ++i)
	{
		auto &channel = peers.peer(i);
		auto &service = *peerServices.emplace_back(std::make_unique<ConnectionService>(channel));

		service.setCallbacks(bench::connectionStatusHandler(channel,
															[&, i](const ConnectionStatusUpdate &update)
															{
																switch (update.type)
																{
																case Type::Established:
																{
																	std::lock_guard<std::mutex> lock(winnerMutex);
																	winners.push_back(i);
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

		service.onPeerValidated(bench::validatedRemote(LoopbackPeers::HubName, peers.hub().getBoundPort()));
		hubService->onPeerValidated(bench::validatedRemote(peers.peerName(i), channel.getBoundPort()));
	}

	peers.start();

	std::optional<bench::BackgroundStream> traffic;
	if (saturated)
		traffic.emplace(peers.peer(0), LoopbackPeers::HubName);

	uint64_t expectedAnswers = 0;
	uint64_t expectedClosed	 = 0;
	uint64_t rounds			 = 0;

	for (auto _ : state)
	{
		const auto start = bench::Clock::now();

		for (const auto &service : peerServices)
			service->initiateConnection(LoopbackPeers::HubName);

		expectedAnswers += peerCount;
		const bool complete = answered.waitFor(expectedAnswers) && hubEstablished.waitFor(++rounds);
		state.SetIterationTime(bench::secondsSince(start));

		std::vector<size_t> won;
		{
			std::lock_guard<std::mutex> lock(winnerMutex);
			won = std::exchange(winners, {});
		}

		if (!complete || won.size() != 1)
		{
			state.SkipWithError("Every invitation must be answered and exactly one session established");
			break;
		}

		// Untimed: the winner closes its session, both sides report it closed
		expectedClosed += 2;
		if (!peerServices[won.front()]->closeConnection(LoopbackPeers::HubName) || !closed.waitFor(expectedClosed))
		{
			state.SkipWithError("The session did not close");
			break;
		}
	}

	// Channel threads call into the services and the services into the channels: stop the threads first
	traffic.reset();
	peers.stop();
	peerServices.clear();
	hubService.reset();
}


// Time: one peer invites, the other accepts, until both sides report the established session
static void BM_ConnectionService_Establish(benchmark::State &state, const bool saturated)
{
	inviteHub(state, 1, saturated);
}
BENCHMARK_CAPTURE(BM_ConnectionService_Establish, IdleChannel, false)->UseManualTime()->MeasureProcessCPUTime()->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(BM_ConnectionService_Establish, SaturatedChannel, true)->UseManualTime()->MeasureProcessCPUTime()->Unit(benchmark::kMillisecond);


// Time: `peers` peers invite one hub at once, until every peer got its answer (one accepted, the rest declined)
static void BM_ConnectionService_InvitationStorm(benchmark::State &state)
{
	inviteHub(state, static_cast<size_t>(state.range(0)), false);
}
BENCHMARK(BM_ConnectionService_InvitationStorm)->ArgName("peers")->Arg(8)->Arg(32)->Arg(128)->UseManualTime()->MeasureProcessCPUTime()->Unit(benchmark::kMillisecond);

} // namespace ConnectionBenchmarks
