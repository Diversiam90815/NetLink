/*
  ==============================================================================
	Module:         ThreadBase
	Description:    Base class for thread management with event triggering
  ==============================================================================
*/

#pragma once

#include <thread>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>

#include "NetLinkLog.h"
#include "Timing/DeadlineTimer.h"


class ThreadBase
{
public:
	ThreadBase() = default;

	virtual ~ThreadBase()
	{
		mRunning.store(false);
		triggerEvent();

		if (mThread.joinable())
			mThread.join();
	}

	virtual void start()
	{
		if (mRunning.exchange(true))
			return; // already running

		if (mThread.joinable())
			mThread.join();

		mThread = std::thread(
			[this]
			{
				try
				{
					run();
				}
				catch (const std::exception &e)
				{
					NETLINK_LOG_ERROR("Worker thread terminated by an exception: {}", e.what());
				}
				catch (...)
				{
					NETLINK_LOG_ERROR("Worker thread terminated by an unknown exception");
				}
			});
	}

	virtual void stop()
	{
		if (!isRunning())
			return;

		mRunning.store(false);
		triggerEvent(); // Wake up the thread
		interruptWork();

		if (mThread.joinable())
			mThread.join();
	}

	// Ends the current waitForEvent(), or the next one if the thread is not waiting right now
	void triggerEvent() { mTimer->wake(); }

	bool isRunning() const { return mRunning.load(); }


protected:
	virtual void run() = 0;

	// Called by stop() once isRunning() is false: wakes the thread from whatever it blocks in besides waitForEvent()
	virtual void interruptWork() {}

	// Waits for triggerEvent(), at most timeoutMS (0 = no limit). True if an event was triggered and the thread is still running.
	bool		 waitForEvent(const unsigned long timeoutMS = 0) const
	{
		if (!isRunning())
			return false;

		const auto deadline		= timeoutMS > 0 ? netlink::IDeadlineTimer::Clock::now() + std::chrono::milliseconds(timeoutMS) : netlink::IDeadlineTimer::Never;
		const bool wasTriggered = mTimer->waitUntil(deadline) == netlink::WaitResult::Woken;

		return wasTriggered && isRunning();
	}


private:
	std::thread								 mThread;							   // Worker thread instance
	std::atomic<bool>						 mRunning{false};					   // Running state flag (set by start()/stop() )
	std::unique_ptr<netlink::IDeadlineTimer> mTimer{netlink::makeDeadlineTimer()}; // Event signaling: the worker waits on it
};