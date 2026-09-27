/*
  ==============================================================================
	Module:         ValidationStoreBenchmarks
	Description:    The thread-safe per-peer stores of the validation flow
  ==============================================================================
*/

#include <benchmark/benchmark.h>

#include <memory>
#include <string>
#include <vector>

#include "BenchUtil.h"
#include "PeerValidation/HandshakeTracker.h"
#include "PeerValidation/PendingValidationStore.h"
#include "PeerValidation/ValidatedPeerRegistry.h"

using namespace netlink;


namespace ValidationBenchmarks
{

static std::string peerName(const int64_t index)
{
	return "peer-" + std::to_string(index);
}


static DiscoveryEndpoint makeEndpoint(const int64_t index)
{
	return DiscoveryEndpoint{
		.IPAddress = net::IPv4Address::fromHostOrder(0x0A000000u + static_cast<uint32_t>(index)), .port = 5000 + static_cast<int>(index % 1000), .displayName = peerName(index)};
}


static ValidationResult makeResult(const int64_t index)
{
	ValidationResult result;
	result.remoteEndpoint = makeEndpoint(index);
	result.status		  = ValidationResult::Status::ReadyToConnect;
	result.message		  = "Peer validated and is ready to connect";
	result.remoteVersion  = "1.4.2.1337";
	result.canConnect	  = true;
	return result;
}


static std::vector<std::string> names(const int64_t count)
{
	std::vector<std::string> result;
	for (int64_t i = 0; i < count; ++i)
		result.push_back(peerName(i));
	return result;
}


// ---------------------------------------------------------------------------
// ValidatedPeerRegistry
// ---------------------------------------------------------------------------

static void BM_ValidatedPeerRegistry_Get(benchmark::State &state)
{
	ValidatedPeerRegistry registry;
	const auto			  peers = names(state.range(0));

	for (int64_t i = 0; i < state.range(0); ++i)
		registry.store(peers[static_cast<size_t>(i)], makeResult(i));

	size_t next = 0;

	for (auto _ : state)
	{
		benchmark::DoNotOptimize(registry.get(peers[next]));
		next = (next + 1) % peers.size();
	}
}
BENCHMARK(BM_ValidatedPeerRegistry_Get)->Apply(bench::populations);


static void BM_ValidatedPeerRegistry_StoreRemove(benchmark::State &state)
{
	ValidatedPeerRegistry registry;

	for (int64_t i = 0; i < state.range(0); ++i)
		registry.store(peerName(i), makeResult(i));

	const std::string peer	 = "DESKTOP-4F2K9Q1";
	const auto		  result = makeResult(state.range(0));

	for (auto _ : state)
	{
		registry.store(peer, result);
		registry.remove(peer);
	}
}
BENCHMARK(BM_ValidatedPeerRegistry_StoreRemove)->Apply(bench::populations);


static void BM_ValidatedPeerRegistry_GetAllReadyToConnect(benchmark::State &state)
{
	ValidatedPeerRegistry registry;

	for (int64_t i = 0; i < state.range(0); ++i)
		registry.store(peerName(i), makeResult(i));

	for (auto _ : state)
		benchmark::DoNotOptimize(registry.getAllReadyToConnect());

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * state.range(0));
}
BENCHMARK(BM_ValidatedPeerRegistry_GetAllReadyToConnect)->Apply(bench::populations);


// One registry read by 1..8 threads at once (channel thread, connection service, API calls)
static std::unique_ptr<ValidatedPeerRegistry> gSharedRegistry;
static constexpr int64_t					  SharedPeers = 64;

static void									  createSharedRegistry(const benchmark::State &)
{
	gSharedRegistry = std::make_unique<ValidatedPeerRegistry>();
	for (int64_t i = 0; i < SharedPeers; ++i)
		gSharedRegistry->store(peerName(i), makeResult(i));
}

static void destroySharedRegistry(const benchmark::State &)
{
	gSharedRegistry.reset();
}

static void BM_ValidatedPeerRegistry_Get_Contended(benchmark::State &state)
{
	const auto peers = names(SharedPeers);
	size_t	   next	 = static_cast<size_t>(state.thread_index());

	for (auto _ : state)
	{
		benchmark::DoNotOptimize(gSharedRegistry->get(peers[next]));
		next = (next + 1) % peers.size();
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_ValidatedPeerRegistry_Get_Contended)->Setup(createSharedRegistry)->Teardown(destroySharedRegistry)->ThreadRange(1, 8)->UseRealTime();


// ---------------------------------------------------------------------------
// PendingValidationStore
// ---------------------------------------------------------------------------

// A validation starts (add), is looked up by every answer (get) and completes (remove)
static void BM_PendingValidationStore_Lifecycle(benchmark::State &state)
{
	PendingValidationStore store;

	for (int64_t i = 0; i < state.range(0); ++i)
		store.add(makeEndpoint(i));

	const auto peer = makeEndpoint(state.range(0));

	for (auto _ : state)
	{
		store.add(peer);
		benchmark::DoNotOptimize(store.get(peer.displayName));
		benchmark::DoNotOptimize(store.get(peer.displayName));
		store.remove(peer.displayName);
	}
}
BENCHMARK(BM_PendingValidationStore_Lifecycle)->Apply(bench::populations);


// ---------------------------------------------------------------------------
// HandshakeTracker
// ---------------------------------------------------------------------------

// Discovered -> sent -> received -> complete, with `entries` other handshakes open
static void BM_HandshakeTracker_Lifecycle(benchmark::State &state)
{
	HandshakeTracker tracker;

	for (int64_t i = 0; i < state.range(0); ++i)
		tracker.beginForDiscoveredPeer(makeEndpoint(i));

	const auto peer = makeEndpoint(state.range(0));

	for (auto _ : state)
	{
		tracker.beginForDiscoveredPeer(peer);
		tracker.markSent(peer.displayName);
		tracker.markReceived(peer.displayName);
		benchmark::DoNotOptimize(tracker.tryCompleteAndRemove(peer.displayName));
	}
}
BENCHMARK(BM_HandshakeTracker_Lifecycle)->Apply(bench::populations);

} // namespace ValidationBenchmarks
