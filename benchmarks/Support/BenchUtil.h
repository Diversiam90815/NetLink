/*
  ==============================================================================
	Module:         BenchUtil
	Description:    Measurement helpers shared by all benchmarks
  ==============================================================================
*/

#pragma once

#include <benchmark/benchmark.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <random>
#include <vector>

#include "Socket/IPv4Address.h"


namespace bench
{

using namespace std::chrono_literals;

using Clock = std::chrono::steady_clock;

// Upper bound for one wait on library work. A benchmark that hits it reports an error instead of hanging.
inline constexpr std::chrono::milliseconds WaitTimeout{30'000};

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


inline double percent(const uint64_t part, const uint64_t whole)
{
	return whole == 0 ? 0.0 : 100.0 * static_cast<double>(part) / static_cast<double>(whole);
}


// Counts completions reported from library threads (callbacks) and lets the benchmark thread wait for them
class CompletionCounter
{
public:
	void notify()
	{
		{
			std::lock_guard<std::mutex> lock(mMutex);
			++mCount;
		}
		mChanged.notify_all();
	}

	// Blocks until at least target completions were counted. False on timeout.
	bool waitFor(const uint64_t target, const std::chrono::milliseconds timeout = WaitTimeout)
	{
		std::unique_lock<std::mutex> lock(mMutex);
		return mChanged.wait_for(lock, timeout, [&] { return mCount >= target; });
	}

	// Like waitFor(), but gives up once the count did not move for `stall`: for flows that may lose work for good
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

private:
	mutable std::mutex		mMutex;
	std::condition_variable mChanged;
	uint64_t				mCount{0};
};

} // namespace bench
