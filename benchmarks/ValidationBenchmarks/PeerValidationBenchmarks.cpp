/*
  ==============================================================================
	Module:         PeerValidationBenchmarks
	Description:    Pre-connection validation (handshake, secret and version
					checks) between production PeerValidationServices on
					production PeerChannels over real UDP, wired like NetLinkCore
  ==============================================================================
*/

#include <benchmark/benchmark.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "BenchUtil.h"
#include "Loopback.h"
#include "ServiceWiring.h"
#include "PeerValidation/PeerValidationService.h"

using namespace netlink;
using bench::LoopbackPeers;


namespace ValidationBenchmarks
{

static DiscoveryEndpoint endpointOf(PeerChannel &channel, const std::string &name)
{
	return DiscoveryEndpoint{.IPAddress = bench::loopback(), .port = channel.getBoundPort(), .displayName = name};
}


// A hub and `peerCount` peers discover each other at the same moment. Each round is timed until the hub validated
// every peer and every peer validated the hub.
static void validateSwarm(benchmark::State &state, const size_t peerCount, const PeerValidationConfig &config)
{
	// Declared first: every thread below reports into these
	bench::CompletionCounter							completed;
	std::atomic<bool>									rejected{false};

	LoopbackPeers										peers(peerCount);
	const auto											hubValidation = std::make_unique<PeerValidationService>();
	std::vector<std::unique_ptr<PeerValidationService>> peerValidations;

	if (!peers.open())
	{
		state.SkipWithError("Could not bind the channels to 127.0.0.1");
		return;
	}

	auto setUp = [&](PeerChannel &channel, PeerValidationService &validation)
	{
		validation.setConfig(config);
		validation.setLocalSecret("a-shared-application-secret");
		validation.setLocalVersion("1.4.2.1337");
		validation.setValidationCallback(
			[&](const ValidationResult &result)
			{
				if (!result.canConnect)
					rejected.store(true);
				completed.notify();
			});
		bench::wireValidation(channel, validation);
	};

	setUp(peers.hub(), *hubValidation);
	for (size_t i = 0; i < peerCount; ++i)
		setUp(peers.peer(i), *peerValidations.emplace_back(std::make_unique<PeerValidationService>()));

	peers.start();

	const auto					   hubEndpoint = endpointOf(peers.hub(), LoopbackPeers::HubName);
	std::vector<DiscoveryEndpoint> peerEndpoints;
	for (size_t i = 0; i < peerCount; ++i)
		peerEndpoints.push_back(endpointOf(peers.peer(i), peers.peerName(i)));

	uint64_t expected = 0;

	for (auto _ : state)
	{
		// Forget the previous round, as NetLinkCore does for re-discovered peers
		for (size_t i = 0; i < peerCount; ++i)
		{
			hubValidation->clearValidatedPeer(peers.peerName(i));
			peerValidations[i]->clearValidatedPeer(LoopbackPeers::HubName);
		}

		const auto start = bench::Clock::now();

		for (size_t i = 0; i < peerCount; ++i)
		{
			hubValidation->onPeerDiscovered(peerEndpoints[i]);
			peerValidations[i]->onPeerDiscovered(hubEndpoint);
		}

		expected += 2 * peerCount;
		const bool complete = completed.waitFor(expected);
		state.SetIterationTime(bench::secondsSince(start));

		if (!complete || rejected.load())
		{
			state.SkipWithError(complete ? "A compatible peer was rejected" : "Validation did not complete for every peer");
			break;
		}
	}

	// No timeout may fire into a stopped channel, and no channel thread into a destroyed service
	hubValidation->cancelAllPendingValidation();
	for (const auto &validation : peerValidations)
		validation->cancelAllPendingValidation();
	peers.stop();
}


// Time: two peers validating each other, per check configuration
static void BM_PeerValidation_Validate(benchmark::State &state, const PeerValidationConfig &config)
{
	validateSwarm(state, 1, config);
}
BENCHMARK_CAPTURE(BM_PeerValidation_Validate, NoChecks, PeerValidationConfig{})
	->UseManualTime()
	->MeasureProcessCPUTime()
	->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_PeerValidation_Validate, Version, PeerValidationConfig{.enableVersionCheck = true})
	->UseManualTime()
	->MeasureProcessCPUTime()
	->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_PeerValidation_Validate, Secret, PeerValidationConfig{.enableSecretCheck = true})
	->UseManualTime()
	->MeasureProcessCPUTime()
	->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_PeerValidation_Validate, SecretAndVersion, PeerValidationConfig{.enableVersionCheck = true, .enableSecretCheck = true})
	->UseManualTime()
	->MeasureProcessCPUTime()
	->Unit(benchmark::kMicrosecond);


// Time: a hub validating `peers` peers that all appear at once (secret and version check). Read items_per_second as peers/s.
static void BM_PeerValidation_Swarm(benchmark::State &state)
{
	const auto peers = static_cast<size_t>(state.range(0));
	validateSwarm(state, peers, PeerValidationConfig{.enableVersionCheck = true, .enableSecretCheck = true});
	state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * peers));
}
BENCHMARK(BM_PeerValidation_Swarm)->ArgName("peers")->Arg(8)->Arg(32)->Arg(128)->Arg(250)->UseManualTime()->MeasureProcessCPUTime()->Unit(benchmark::kMillisecond);

} // namespace ValidationBenchmarks
