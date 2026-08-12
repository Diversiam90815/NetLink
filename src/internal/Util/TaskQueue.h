/*
  ==============================================================================
	Module:         TaskQueue
	Description:    Single-threaded, serial FIFO task queue used to marshal
					 callbacks off their originating thread (e.g. signaling/IO)
					 onto a dedicated worker thread, guaranteeing in-order,
					 non-reentrant execution.
  ==============================================================================
*/

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>


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

			task();
		}
	}

	std::thread				mThread;
	std::atomic<bool>		mRunning{false};
	mutable std::mutex		mMutex;
	std::condition_variable mCV;
	std::queue<Task>		mQueue;
};
