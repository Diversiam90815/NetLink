/*
  ==============================================================================
	Module:         PeerValidationBenchmarks
	Description:    The complete pre-connection validation between two peers.
					Load: a hub validating up to 250 peers that appear at once.
  ==============================================================================
*/

#include <benchmark/benchmark.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "BenchUtil.h"
#include "LoopbackChannelPair.h"
#include "LoopbackSwarm.h"
#include "ServiceWiring.h"
#include "PeerValidation/PeerValidationService.h"

using namespace netlink;
using bench::LoopbackChannelPair;
using bench::LoopbackSwarm;


namespace ValidationBenchmarks
{

// arg 0: no checks, 1: version, 2: secret, 3: both
static PeerValidationConfig configFor(const int64_t checks)
{
	PeerValidationConfig config;
	config.enableVersionCheck = (checks & 1) != 0;
	config.enableSecretCheck  = (checks & 2) != 0;
	return config;
}


static const char *labelFor(const int64_t checks)
{
	switch (checks)
	{
	case 0: return "no checks";
	case 1: return "version";
	case 2: return "secret";
	default: return "secret + version";
	}
}


// Configured as NetLinkCore configures it, with the checks selected by `checks`
static void configure(PeerValidationService &validation, const int64_t checks)
{
	validation.setConfig(configFor(checks));
	validation.setLocalSecret("a-shared-application-secret");
	validation.setLocalVersion("1.4.2.1337");
}


// Every result is counted; a rejection of these always compatible peers is remembered
static void reportResults(PeerValidationService &validation, bench::CompletionCounter &completed, std::atomic<bool> &rejected)
{
	validation.setValidationCallback(
		[&completed, &rejected](const ValidationResult &result)
		{
			if (!result.canConnect)
				rejected.store(true);

			completed.notify();
		});
}


// How a peer's discovery announcement describes its channel
static DiscoveryEndpoint endpointOf(const PeerChannel &channel, const std::string &name)
{
	return DiscoveryEndpoint{.IPAddress = bench::loopback(), .port = channel.getBoundPort(), .displayName = name};
}


class BM_PeerValidation : public benchmark::Fixture
{
public:
	using benchmark::Fixture::SetUp;
	using benchmark::Fixture::TearDown;

	void SetUp(benchmark::State &state) override
	{
		completed.reset();
		rejected.store(false);

		pair = std::make_unique<LoopbackChannelPair>();

		if (!pair->open())
		{
			state.SkipWithError("Could not bind the peer channels to 127.0.0.1");
			return;
		}

		validationA = std::make_unique<PeerValidationService>();
		validationB = std::make_unique<PeerValidationService>();

		wire(pair->a, *validationA, state.range(0));
		wire(pair->b, *validationB, state.range(0));

		pair->start();
	}

	void TearDown(benchmark::State &) override
	{
		// No timeout may fire into a channel that is gone, and no channel thread into a service that is gone
		if (validationA)
			validationA->cancelAllPendingValidation();
		if (validationB)
			validationB->cancelAllPendingValidation();

		pair.reset();
		validationA.reset();
		validationB.reset();
	}

protected:
	void wire(PeerChannel &channel, PeerValidationService &validation, const int64_t checks)
	{
		configure(validation, checks);
		reportResults(validation, completed, rejected);
		bench::wireValidation(channel, validation);
	}

	bench::CompletionCounter			   completed;
	std::atomic<bool>					   rejected{false};

	std::unique_ptr<PeerValidationService> validationA;
	std::unique_ptr<PeerValidationService> validationB;
	std::unique_ptr<LoopbackChannelPair>   pair;
};


// Both peers discover each other; timed until both reported a validation result
BENCHMARK_DEFINE_F(BM_PeerValidation, Validate)(benchmark::State &state)
{
	const auto remoteOfA = endpointOf(pair->b, LoopbackChannelPair::NameB);
	const auto remoteOfB = endpointOf(pair->a, LoopbackChannelPair::NameA);
	uint64_t   expected	 = 0;

	for (auto _ : state)
	{
		// Forget the previous round, as NetLinkCore does for a re-discovered peer
		validationA->clearValidatedPeer(LoopbackChannelPair::NameB);
		validationB->clearValidatedPeer(LoopbackChannelPair::NameA);

		const auto start = bench::Clock::now();

		validationA->onPeerDiscovered(remoteOfA);
		validationB->onPeerDiscovered(remoteOfB);

		expected += 2;

		if (!completed.waitFor(expected))
		{
			state.SkipWithError("Validation did not complete");
			return;
		}

		state.SetIterationTime(bench::secondsSince(start));

		if (rejected.load())
		{
			state.SkipWithError("A peer was rejected although both are compatible");
			return;
		}
	}

	state.SetLabel(labelFor(state.range(0)));
	state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK_REGISTER_F(BM_PeerValidation, Validate)->ArgName("checks")->Arg(0)->Arg(1)->Arg(2)->Arg(3)->UseManualTime();


// Many peers appear at once: a hub and N peers (secret and version check) all discover each other at the same moment.
// Timed until the hub validated every peer and every peer validated the hub; items: peers validated by the hub.
static void BM_PeerValidation_Swarm(benchmark::State &state)
{
	constexpr int64_t									bothChecks = 3;
	const auto											peerCount  = static_cast<size_t>(state.range(0));

	// Declared first: every thread below reports into these
	bench::CompletionCounter							completed;
	std::atomic<bool>									rejected{false};

	LoopbackSwarm										swarm(peerCount);
	auto												hubValidation = std::make_unique<PeerValidationService>();
	std::vector<std::unique_ptr<PeerValidationService>> peerValidations;

	if (!swarm.open())
	{
		state.SkipWithError("Could not bind the swarm to 127.0.0.1");
		return;
	}

	auto setUp = [&](PeerChannel &channel, PeerValidationService &validation)
	{
		configure(validation, bothChecks);
		reportResults(validation, completed, rejected);
		bench::wireValidation(channel, validation);
	};

	setUp(swarm.hub, *hubValidation);

	for (size_t i = 0; i < peerCount; ++i)
		setUp(*swarm.peers[i], *peerValidations.emplace_back(std::make_unique<PeerValidationService>()));

	swarm.start();

	const auto					   hubEndpoint = endpointOf(swarm.hub, LoopbackSwarm::HubName);
	std::vector<DiscoveryEndpoint> peerEndpoints;
	for (size_t i = 0; i < peerCount; ++i)
		peerEndpoints.push_back(endpointOf(*swarm.peers[i], swarm.names[i]));

	uint64_t expected = 0;

	for (auto _ : state)
	{
		// Forget the previous round, as NetLinkCore does for re-discovered peers
		for (size_t i = 0; i < peerCount; ++i)
		{
			hubValidation->clearValidatedPeer(swarm.names[i]);
			peerValidations[i]->clearValidatedPeer(LoopbackSwarm::HubName);
		}

		const auto start = bench::Clock::now();

		for (size_t i = 0; i < peerCount; ++i)
		{
			hubValidation->onPeerDiscovered(peerEndpoints[i]);
			peerValidations[i]->onPeerDiscovered(hubEndpoint);
		}

		expected += 2 * peerCount;
		const bool complete = completed.waitFor(expected, bench::LoadTimeout);
		state.SetIterationTime(bench::secondsSince(start));

		if (!complete || rejected.load())
		{
			state.SkipWithError(complete ? "A compatible peer was rejected" : "Validation did not complete for every peer");
			break;
		}
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * peerCount));

	// No timeout may fire into a stopped swarm, and no channel thread into a destroyed service
	hubValidation->cancelAllPendingValidation();
	for (const auto &validation : peerValidations)
		validation->cancelAllPendingValidation();
	swarm.stop();
}
BENCHMARK(BM_PeerValidation_Swarm)->ArgName("peers")->Arg(8)->Arg(32)->Arg(128)->Arg(250)->UseManualTime()->Unit(benchmark::kMillisecond);

} // namespace ValidationBenchmarks
