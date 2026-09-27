/*
  ==============================================================================
	Module:         BenchUtil
	Description:    Measurement helpers shared by all benchmarks: deterministic
					payloads, completion waiting and common argument sets.
  ==============================================================================
*/

#pragma once

#include <benchmark/benchmark.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <random>
#include <vector>

#include "Socket/IPv4Address.h"


namespace bench
{

using namespace std::chrono_literals;

using Clock = std::chrono::steady_clock;

// Upper bound for any single wait on asynchronous library work. A benchmark that hits it reports an error instead of hanging.
inline constexpr std::chrono::milliseconds CompletionTimeout{5000};

// The same bound for load benchmarks, whose single waits cover whole bursts, streams or swarms
inline constexpr std::chrono::milliseconds LoadTimeout{60000};

// Size units for argument lists
inline constexpr int64_t				   KiB = 1024;
inline constexpr int64_t				   MiB = 1024 * KiB;


// Deterministic bytes, so runs stay comparable across builds and machines
inline std::vector<uint8_t>				   makePayload(const size_t size, const uint32_t seed = 0x4E4C)
{
	std::vector<uint8_t> payload(size);
	std::mt19937		 random(seed);

	for (auto &byte : payload)
		byte = static_cast<uint8_t>(random());

	return payload;
}


// 127.0.0.host
constexpr netlink::net::IPv4Address loopback(const uint8_t host = 1)
{
	return netlink::net::IPv4Address::fromHostOrder(0x7F000000u | host);
}


inline double secondsSince(const Clock::time_point start)
{
	return std::chrono::duration<double>(Clock::now() - start).count();
}


// Counts completions reported from library threads (callbacks) and lets the benchmark thread wait for them
class CompletionCounter
{
public:
	void notify(const uint64_t count = 1)
	{
		{
			std::lock_guard<std::mutex> lock(mMutex);
			mCount += count;
		}
		mChanged.notify_all();
	}

	// Blocks until at least target completions were counted. False on timeout.
	bool waitFor(const uint64_t target, const std::chrono::milliseconds timeout = CompletionTimeout)
	{
		std::unique_lock<std::mutex> lock(mMutex);
		return mChanged.wait_for(lock, timeout, [&] { return mCount >= target; });
	}

	// Blocks until at least target completions were counted, as long as the count keeps moving. False once it stalled for `stall`.
	bool waitWhileProgressing(const uint64_t target, const std::chrono::milliseconds stall)
	{
		std::unique_lock<std::mutex> lock(mMutex);

		while (mCount < target)
		{
			const uint64_t before = mCount;
			if (!mChanged.wait_for(lock, stall, [&] { return mCount != before; }))
				return false;
		}

		return true;
	}

	uint64_t value() const
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return mCount;
	}

	void reset()
	{
		std::lock_guard<std::mutex> lock(mMutex);
		mCount = 0;
	}

private:
	mutable std::mutex		mMutex;
	std::condition_variable mChanged;
	uint64_t				mCount{0};
};


// ---------------------------------------------------------------------------
// Common argument sets (->Apply(...))
// ---------------------------------------------------------------------------

// Single datagrams: tiny, typical, the channel's MTU-safe maximum, and large loopback datagrams
inline void datagramSizes(benchmark::internal::Benchmark *b)
{
	b->ArgName("bytes");
	for (const int64_t size : {64, 512, 1200, 8 * 1024, 60 * 1024})
		b->Arg(size);
}


// Whole messages: single datagram, a few fragments, many fragments
inline void messageSizes(benchmark::internal::Benchmark *b)
{
	b->ArgName("bytes");
	for (const int64_t size : {64, 1024, 64 * 1024, 1024 * 1024})
		b->Arg(size);
}


// Number of entries held by a container or service
inline void populations(benchmark::internal::Benchmark *b)
{
	b->ArgName("entries");
	for (const int64_t count : {8, 64, 512, 4096})
		b->Arg(count);
}


// Messages per timed batch: keeps the data per iteration around 1 MiB, at least 1 and at most 64 messages
inline size_t batchFor(const size_t messageSize)
{
	const size_t batch = (size_t{1024} * 1024) / (messageSize == 0 ? 1 : messageSize);
	return batch < 1 ? 1 : (batch > 64 ? 64 : batch);
}

} // namespace bench
