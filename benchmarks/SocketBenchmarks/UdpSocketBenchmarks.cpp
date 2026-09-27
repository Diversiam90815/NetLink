/*
  ==============================================================================
	Module:         UdpSocketBenchmarks
	Description:    Real UDP sockets on the loopback interface
  ==============================================================================
*/

#include <benchmark/benchmark.h>

#include <algorithm>
#include <atomic>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "BenchUtil.h"
#include "Socket/UdpSocket.h"
#include "NetLinkConstants.h"

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


// Receives on a socket in the background until stopped. With echo, every datagram is sent back to its sender.
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
			const auto datagram = mSocket.receiveFrom(buffer, std::chrono::milliseconds{10});
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


// Send-side throughput. A background receiver drains the socket; what the OS drops under load shows in the delivered counter.
static void BM_UdpSocket_SendTo(benchmark::State &state)
{
	auto receiver = bindLoopback(state);
	auto sender	  = bindLoopback(state);
	if (!receiver || !sender)
		return;

	const auto		payload = bench::makePayload(static_cast<size_t>(state.range(0)));
	const auto		target	= receiver->localAddress();
	uint64_t		failed	= 0;

	const Responder drain(*receiver, false);

	for (auto _ : state)
	{
		if (!sender->sendTo(target, payload))
			++failed;
	}

	const auto sent			  = static_cast<int64_t>(state.iterations());
	state.counters["failed"]  = static_cast<double>(failed);
	state.counters["arrived"] = benchmark::Counter(static_cast<double>(drain.received()) / static_cast<double>(std::max<int64_t>(sent, 1)));
	state.SetItemsProcessed(sent);
	state.SetBytesProcessed(sent * state.range(0));
}
BENCHMARK(BM_UdpSocket_SendTo)->Apply(bench::datagramSizes)->UseRealTime();


// Round trip: send, the peer echoes, block until the echo arrived
static void BM_UdpSocket_PingPong(benchmark::State &state)
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
		if (auto sent = client->sendTo(target, payload); !sent)
		{
			state.SkipWithError("sendTo failed: " + std::string(toString(sent.error())));
			break;
		}

		if (auto reply = client->receiveFrom(buffer, bench::CompletionTimeout); !reply)
		{
			state.SkipWithError("No echo received: " + std::string(toString(reply.error())));
			break;
		}
	}

	state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
	state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * state.range(0) * 2);
}
BENCHMARK(BM_UdpSocket_PingPong)->Apply(bench::datagramSizes)->UseRealTime();


// An idle wait: what every I/O loop pays per poll interval when nothing arrives
static void BM_UdpSocket_ReceiveTimeout(benchmark::State &state)
{
	auto socket = bindLoopback(state);
	if (!socket)
		return;

	std::vector<uint8_t> buffer(64);
	const auto			 timeout = std::chrono::milliseconds{state.range(0)};

	for (auto _ : state)
	{
		auto result = socket->receiveFrom(buffer, timeout);
		benchmark::DoNotOptimize(result);
	}
}
BENCHMARK(BM_UdpSocket_ReceiveTimeout)->ArgName("timeoutMs")->Arg(0)->Arg(1)->UseRealTime();


// Socket creation, option setup and bind, and closing it again
static void BM_UdpSocket_Bind(benchmark::State &state)
{
	for (auto _ : state)
	{
		auto socket = UdpSocket::bind({.ip = bench::loopback(), .port = 0});
		if (!socket)
		{
			state.SkipWithError("bind failed: " + std::string(toString(socket.error())));
			break;
		}
		benchmark::DoNotOptimize(socket);
	}
}
BENCHMARK(BM_UdpSocket_Bind)->UseRealTime();

} // namespace SocketBenchmarks
