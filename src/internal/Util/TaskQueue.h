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

		mThread = std::thread(&TaskQueue::run, this);
	}

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

		if (mThread.joinable())
			mThread.join();
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


private:
	void run()
	{
		while (true)
		{
			Task task;
			{
				std::unique_lock<std::mutex> lock(mMutex);
				mCV.wait(lock, [this] { return !mQueue.empty() || !mRunning.load(); });

				if (!mRunning.load())
					return;

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
	mutable std::mutex		mMutex;
	std::condition_variable mCV;
	std::queue<Task>		mQueue;
};
