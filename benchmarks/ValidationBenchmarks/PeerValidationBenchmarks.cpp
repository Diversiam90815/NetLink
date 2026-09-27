/*
  ==============================================================================
	Module:         PeerValidationBenchmarks
	Description:    The complete pre-connection validation between two peers
  ==============================================================================
*/

#include <benchmark/benchmark.h>

#include <atomic>
#include <memory>
#include <string>

#include "BenchUtil.h"
#include "LoopbackChannelPair.h"
#include "PeerValidation/PeerValidationService.h"

using namespace netlink;
using bench::LoopbackChannelPair;


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
	// Same wiring as NetLinkCore::wireServices()
	void wire(PeerChannel &channel, PeerValidationService &validation, const int64_t checks)
	{
		validation.setConfig(configFor(checks));
		validation.setLocalSecret("a-shared-application-secret");
		validation.setLocalVersion("1.4.2.1337");

		ChannelValidationCallbacks signals;
		signals.onValidationRequestReceived = [&validation](const std::string &name, const RemoteRequest request) { validation.onRequestReceived(name, request); };
		signals.onSecretResponseReceived	= [&validation](const std::string &name, const std::string &secret)
		{ validation.onCheckResponseReceived(name, RemoteRequest::Secret, secret); };
		signals.onVersionResponseReceived = [&validation](const std::string &name, const std::string &version)
		{ validation.onCheckResponseReceived(name, RemoteRequest::Version, version); };
		signals.onValidationHandshakeReceived = [&validation](const std::string &name) { validation.onHandshakeReceived(name); };
		channel.setValidationCallbacks(std::move(signals));

		PeerValidationSendCallbacks send;
		send.sendRequest		 = [&channel](const std::string &name, const RemoteRequest request) { channel.sendValidationRequest(name, request); };
		send.sendSecretResponse	 = [&channel](const std::string &name, const std::string &value) { channel.sendSecretResponse(name, value); };
		send.sendVersionResponse = [&channel](const std::string &name, const std::string &value) { channel.sendVersionResponse(name, value); };
		send.sendHandshake		 = [&channel](const std::string &name) { channel.sendValidationHandshake(name); };
		validation.setSendCallbacks(std::move(send));

		validation.setValidationCallback(
			[this](const ValidationResult &result)
			{
				if (!result.canConnect)
					rejected.store(true);

				completed.notify();
			});
	}

	static DiscoveryEndpoint endpointOf(const PeerChannel &channel, const char *name)
	{
		return DiscoveryEndpoint{.IPAddress = bench::loopback(), .port = channel.getBoundPort(), .displayName = name};
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

} // namespace ValidationBenchmarks
