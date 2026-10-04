/*
  ==============================================================================
	Module:         SimDriver
	Description:    Steps engines through virtual time on one thread: the same
					I/O loop as in production, but deterministic and as fast as
					the test machine computes
  ==============================================================================
*/

#pragma once

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "Engine/NetworkEngine.h"
#include "FakeDatagramNetwork.h"


namespace FakeNet
{

// The engines must not run on a thread: the driver runs their loops and hands their events to the sink they were
// added with. Their sockets come from the network it is given.
class SimDriver
{
public:
	using Sink = std::function<void(netlink::EventBatch &&)>;

	explicit SimDriver(std::shared_ptr<FakeDatagramNetwork> network) : mNetwork(std::move(network)) { mNetwork->useVirtualTime(mNow); }

	void add(netlink::NetworkEngine &engine, Sink sink) { mEngines.push_back({&engine, std::move(sink)}); }

	// The engine is not stepped anymore, like a machine that froze
	void remove(const netlink::NetworkEngine &engine)
	{
		std::erase_if(mEngines, [&](const auto &entry) { return entry.first == &engine; });
	}

	// Called before and after every step of an engine, e.g. to count what that engine allocates
	std::function<void(const netlink::NetworkEngine &engine, bool entering)> aroundStep;

	TimePoint				  now() const { return mNow; }
	std::chrono::microseconds elapsed() const { return std::chrono::duration_cast<std::chrono::microseconds>(mNow - mStart); }

	void					  run(const std::chrono::microseconds duration)
	{
		runUntil([] { return false; }, duration);
	}

	// Lets time pass until done() holds, at most for the given time. Returns whether it held.
	template <typename Done>
	bool runUntil(Done &&done, const std::chrono::microseconds limit)
	{
		const TimePoint end = mNow + limit;

		while (true)
		{
			const size_t received = mNetwork->receivedCount();
			const auto	 wake	  = settle();

			if (done())
				return true;

			if (mNow >= end)
				return false;

			TimePoint next = end;

			// A timer that is still due is retried a little later, like the real loop does
			if (wake)
				next = std::min(next, *wake > mNow ? *wake : mNow + std::chrono::milliseconds{1});

			// A datagram that waits although nothing was read belongs to a socket no engine of this driver reads
			const bool stuck = mNetwork->receivedCount() == received;

			if (const auto arrival = mNetwork->nextArrival(stuck ? std::optional{mNow} : std::nullopt))
				next = std::min(next, std::max(*arrival, mNow));

			mNow = next;
			mNetwork->advanceTo(mNow);
		}
	}

private:
	// Everything that happens at the current instant. Returns the earliest time an engine wants to run again.
	std::optional<TimePoint> settle()
	{
		std::optional<TimePoint> wake;
		bool					 again = true;

		while (again)
		{
			const size_t received = mNetwork->receivedCount();
			wake.reset();

			for (auto &[engine, sink] : mEngines)
			{
				if (aroundStep)
					aroundStep(*engine, true);

				auto batch = engine->step(mNow);

				if (aroundStep)
					aroundStep(*engine, false);

				sink(std::move(batch));

				if (const auto next = engine->nextWake(); next && (!wake || *next < *wake))
					wake = next;
			}

			// Another round if an engine was given work, or took in datagrams and may have answered them
			const auto arrival = mNetwork->nextArrival();
			again			   = mNetwork->takeInterrupts() || (mNetwork->receivedCount() != received && arrival && *arrival <= mNow);
		}

		return wake;
	}

	std::shared_ptr<FakeDatagramNetwork> mNetwork;
	std::vector<std::pair<netlink::NetworkEngine *, Sink>> mEngines;
	TimePoint							 mStart = Clock::now();
	TimePoint							 mNow	= mStart;
};

} // namespace FakeNet
