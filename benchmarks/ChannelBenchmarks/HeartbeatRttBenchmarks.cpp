/*
  ==============================================================================
	Module:         HeartbeatRttBenchmarks
	Description:    Liveness bookkeeping per packet and per timer tick, and the
					RTT estimator updated by every acknowledgement
  ==============================================================================
*/

#include <benchmark/benchmark.h>

#include <vector>

#include "BenchUtil.h"
#include "Channel/Heartbeat/HeartbeatService.h"
#include "Channel/Reliability/RttEstimator.h"

using namespace netlink;
using namespace netlink::channel;


namespace ChannelBenchmarks
{

static std::vector<net::SocketAddress> watchPeers(HeartbeatService &service, const size_t count, const HeartbeatService::TimePoint now)
{
	std::vector<net::SocketAddress> peers;

	for (size_t i = 0; i < count; ++i)
	{
		peers.push_back({.ip = bench::loopback(), .port = static_cast<uint16_t>(50000 + i)});
		service.watch(peers.back(), now);
	}

	return peers;
}


// Every received datagram refreshes its peer
static void BM_HeartbeatService_OnReceived(benchmark::State &state)
{
	HeartbeatService service;
	auto			 now   = HeartbeatService::Clock::now();
	const auto		 peers = watchPeers(service, static_cast<size_t>(state.range(0)), now);
	size_t			 next  = 0;

	for (auto _ : state)
	{
		now += std::chrono::microseconds{10};
		service.onReceived(peers[next], now);
		next = (next + 1) % peers.size();
	}
}
BENCHMARK(BM_HeartbeatService_OnReceived)->ArgName("peers")->Arg(1)->Arg(16)->Arg(256);


// A timer tick in which every peer is due for a heartbeat and none has gone silent
static void BM_HeartbeatService_Tick(benchmark::State &state)
{
	HeartbeatService service;
	auto			 now   = HeartbeatService::Clock::now();
	const auto		 peers = watchPeers(service, static_cast<size_t>(state.range(0)), now);

	for (auto _ : state)
	{
		now += service.config().interval;

		for (const auto &peer : peers)
			service.onReceived(peer, now);

		auto tick = service.tick(now);
		benchmark::DoNotOptimize(tick);
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * peers.size()));
}
BENCHMARK(BM_HeartbeatService_Tick)->ArgName("peers")->Arg(1)->Arg(16)->Arg(256);


// Asked by the I/O loop before every wait
static void BM_HeartbeatService_NextDeadline(benchmark::State &state)
{
	HeartbeatService service;
	watchPeers(service, static_cast<size_t>(state.range(0)), HeartbeatService::Clock::now());

	for (auto _ : state)
		benchmark::DoNotOptimize(service.nextDeadline());
}
BENCHMARK(BM_HeartbeatService_NextDeadline)->ArgName("peers")->Arg(1)->Arg(16)->Arg(256);


static void BM_RttEstimator_AddSample(benchmark::State &state)
{
	RttEstimator estimator(std::chrono::milliseconds{100}, std::chrono::milliseconds{20}, std::chrono::milliseconds{1000});
	int64_t		 sample = 0;

	for (auto _ : state)
	{
		estimator.addSample(RttEstimator::Duration{200 + (sample++ % 64)});
		benchmark::DoNotOptimize(estimator.rto());
	}
}
BENCHMARK(BM_RttEstimator_AddSample);


static void BM_RttEstimator_TimeoutFor(benchmark::State &state)
{
	const RttEstimator estimator(std::chrono::milliseconds{100}, std::chrono::milliseconds{20}, std::chrono::milliseconds{1000});
	int				   attempt = static_cast<int>(state.range(0));

	for (auto _ : state)
	{
		benchmark::DoNotOptimize(attempt);
		benchmark::DoNotOptimize(estimator.timeoutFor(attempt));
	}
}
BENCHMARK(BM_RttEstimator_TimeoutFor)->ArgName("attempt")->Arg(0)->Arg(3)->Arg(8);

} // namespace ChannelBenchmarks
