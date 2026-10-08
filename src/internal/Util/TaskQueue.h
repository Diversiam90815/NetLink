/*
  ==============================================================================
	Module:         TaskQueue
	Description:    Single-threaded, serial FIFO task queue
  ==============================================================================
*/

#pragma once

#include <atomic>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

#include "NetLinkLog.h"
#include "ThreadUtils.h"


class TaskQueue
{
public:
	using Task	= std::function<void()>;

	TaskQueue() = default;
	~TaskQueue() { stop(); }

	TaskQueue(const TaskQueue &)			= delete;
	TaskQueue &operator=(const TaskQueue &) = delete;

	void	   start()
	{
		if (mRunning.exchange(true))
			return;

		mDraining = false;
		netlink::internal::threadsStarted.fetch_add(1);
		mThread = std::thread(&TaskQueue::run, this);
	}

	// Discards the tasks that did not start yet
	void stop()
	{
		if (!mRunning.exchange(false))
			return;

		{
			std::lock_guard<std::mutex> lock(mMutex);
			std::queue<Task>			empty;
			std::swap(mQueue, empty);
		}
		mCV.notify_all();

		joinOrDetach(mThread);
	}

	// Runs the tasks that are queued, then stops
	void stopAfterDrain()
	{
		{
			std::lock_guard<std::mutex> lock(mMutex);

			if (!mRunning.load())
				return;

			mDraining = true;
		}
		mCV.notify_all();

		joinOrDetach(mThread);
	}

	// Enqueues a task for execution on the worker thread (FIFO order).
	void post(Task task)
	{
		{
			std::lock_guard<std::mutex> lock(mMutex);
			mQueue.push(std::move(task));
		}
		mCV.notify_one();
	}

	// Number of tasks currently pending (not yet started).
	size_t pending() const
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return mQueue.size();
	}

	bool isRunning() const { return mRunning.load(); }

	// Whether the caller is a task of this queue
	bool isWorkerThread() const { return mThread.get_id() == std::this_thread::get_id(); }


private:
	void run()
	{
		while (true)
		{
			Task task;
			{
				std::unique_lock<std::mutex> lock(mMutex);
				mCV.wait(lock, [this] { return !mQueue.empty() || !mRunning.load() || mDraining; });

				if (!mRunning.load())
					return;

				if (mQueue.empty())
				{
					mRunning.store(false);
					return;
				}

				task = std::move(mQueue.front());
				mQueue.pop();
			}

			try
			{
				task();
			}
			catch (const std::exception &e)
			{
				NETLINK_LOG_ERROR("Task threw an exception, ignoring it: {}", e.what());
			}
			catch (...)
			{
				NETLINK_LOG_ERROR("Task threw an unknown exception, ignoring it");
			}
		}
	}

	std::thread				mThread;
	std::atomic<bool>		mRunning{false};
	bool					mDraining{false}; // guarded by mMutex
	mutable std::mutex		mMutex;
	std::condition_variable mCV;
	std::queue<Task>		mQueue;
};
