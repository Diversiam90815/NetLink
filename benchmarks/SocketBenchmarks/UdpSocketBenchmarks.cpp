/*
  ==============================================================================
	Module:         UdpSocketBenchmarks
	Description:    Raw UDP on the loopback interface: the operating system's
					floor underneath every NetLink measurement
  ==============================================================================
*/

#include <benchmark/benchmark.h>

#include <atomic>
#include <latch>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "BenchUtil.h"
#include "NetLinkConstants.h"
#include "Socket/UdpSocket.h"

using namespace netlink::net;


namespace SocketBenchmarks
{

static std::optional<UdpSocket> bindLoopback(benchmark::State &state)
{
	auto socket = UdpSocket::bind({.ip = bench::loopback(), .port = 0});

	if (!socket)
	{
		state.SkipWithError("Binding a loopback socket failed: " + std::string(toString(socket.error())));
		return std::nullopt;
	}

	return std::move(*socket);
}


// Receives on a socket in the background until destroyed. With echo, every datagram is sent back to its sender.
class Responder
{
public:
	Responder(UdpSocket &socket, const bool echo) : mSocket(socket), mEcho(echo), mThread([this] { run(); }) {}

	~Responder()
	{
		mStop.store(true);
		mThread.join();
	}

	Responder(const Responder &)			= delete;
	Responder &operator=(const Responder &) = delete;

	uint64_t   received() const { return mReceived.load(); }

private:
	void run()
	{
		std::vector<uint8_t> buffer(netlink::internal::PackageBufferSize);

		while (!mStop.load())
		{
			auto datagram = mSocket.receiveFrom(buffer, std::chrono::milliseconds{10});
			if (!datagram)
				continue;

			mReceived.fetch_add(1, std::memory_order_relaxed);

			if (mEcho)
				static_cast<void>(mSocket.sendTo(datagram->from, std::span<const uint8_t>(buffer.data(), datagram->size)));
		}
	}

	UdpSocket			 &mSocket;
	bool				  mEcho;
	std::atomic<bool>	  mStop{false};
	std::atomic<uint64_t> mReceived{0};
	std::thread			  mThread;
};


// Time: one datagram to an echoing socket and back
static void BM_UdpSocket_RoundTrip(benchmark::State &state)
{
	auto server = bindLoopback(state);
	auto client = bindLoopback(state);
	if (!server || !client)
		return;

	const auto			 payload = bench::makePayload(static_cast<size_t>(state.range(0)));
	const auto			 target	 = server->localAddress();
	std::vector<uint8_t> buffer(netlink::internal::PackageBufferSize);

	Responder			 echo(*server, true);

	for (auto _ : state)
	{
		if (!client->sendTo(target, payload) || !client->receiveFrom(buffer, bench::WaitTimeout))
		{
			state.SkipWithError("No echo arrived");
			break;
		}
	}
}
BENCHMARK(BM_UdpSocket_RoundTrip)->ArgName("bytes")->Arg(64)->Arg(1200)->UseRealTime()->MeasureProcessCPUTime()->Unit(benchmark::kMicrosecond);


// Time: N sockets each fire 10k datagrams of the channel's datagram size at one receiver.
// delivered_pct: share the receiver got; the operating system drops the rest when its receive buffer overflows.
static void BM_UdpSocket_FanIn(benchmark::State &state)
{
	constexpr uint64_t PerSender = 10'000;
	const auto		   senders	 = static_cast<size_t>(state.range(0));
	const auto		   payload	 = bench::makePayload(netlink::internal::MaxDatagramSize);

	auto			   receiver	 = bindLoopback(state);
	if (!receiver)
		return;

	std::vector<UdpSocket> sockets;
	for (size_t s = 0; s < senders; ++s)
	{
		auto socket = bindLoopback(state);
		if (!socket)
			return;
		sockets.push_back(std::move(*socket));
	}

	const auto target = receiver->localAddress();
	Responder  drain(*receiver, false);

	for (auto _ : state)
	{
		std::latch				 go(1);
		std::vector<std::thread> threads;

		for (auto &socket : sockets)
		{
			threads.emplace_back(
				[&]
				{
					go.wait();
					for (uint64_t i = 0; i < PerSender; ++i)
						static_cast<void>(socket.sendTo(target, payload));
				});
		}

		const auto start = bench::Clock::now();
		go.count_down();

		for (auto &thread : threads)
			thread.join();

		state.SetIterationTime(bench::secondsSince(start));
	}

	std::this_thread::sleep_for(std::chrono::milliseconds{200}); // untimed: let the receiver drain what is still queued

	const auto sent				    = static_cast<uint64_t>(state.iterations()) * senders * PerSender;
	state.counters["delivered_pct"] = bench::percent(drain.received(), sent);
	state.SetItemsProcessed(static_cast<int64_t>(sent));
	state.SetBytesProcessed(static_cast<int64_t>(sent * payload.size()));
}
BENCHMARK(BM_UdpSocket_FanIn)->ArgName("senders")->Arg(1)->Arg(8)->Arg(32)->UseManualTime()->MeasureProcessCPUTime()->Unit(benchmark::kMillisecond);

} // namespace SocketBenchmarks
