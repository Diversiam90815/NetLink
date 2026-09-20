/*
  ==============================================================================
	Module:         ThreadBase
	Description:    Base class for thread management with event triggering
  ==============================================================================
*/

#pragma once

#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <exception>

#include "NetLinkLog.h"


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

	void start()
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

		if (mThread.joinable())
			mThread.join();
	}

	void triggerEvent()
	{
		{
			std::lock_guard<std::mutex> lock(mMutex);
			mEventTriggered = true;
		}
		cv.notify_one();
	}

	bool isRunning() const { return mRunning.load(); }


protected:
	virtual void run() = 0;

	bool		 waitForEvent(unsigned long timeoutMS = 0)
	{
		std::unique_lock<std::mutex> lock(mMutex);

		if (timeoutMS > 0)
		{
			cv.wait_for(lock, std::chrono::milliseconds(timeoutMS), [this] { return mEventTriggered || !isRunning(); });
		}
		else
		{
			cv.wait(lock, [this] { return mEventTriggered || !isRunning(); });
		}
		bool wasTriggered = mEventTriggered;
		mEventTriggered	  = false;			// Reset the flag

		return wasTriggered && isRunning(); // Return true if event was triggered and thread is still running
	}


private:
	std::thread				mThread;				// Worker thread instance
	std::atomic<bool>		mRunning{false};		// Running state flag (set by start()/stop() )
	std::mutex				mMutex;					// Protect event flag
	std::condition_variable cv;						// Condition variable for event signaling
	bool					mEventTriggered{false}; // Indicates an event has been triggerd
};