/*
  ==============================================================================
	Module:         TaskQueue
	Description:    Single-threaded, serial FIFO task queue used to marshal
					 callbacks off their originating thread onto a dedicated worker 
					 thread, guaranteeing in-order, non-reentrant execution.
  ==============================================================================
*/

#pragma once

#include <functional>
#include <queue>
#include <mutex>

#include "ThreadBase.h"


class TaskQueue : public ThreadBase
{
public:
	using Task	= std::function<void()>;

	TaskQueue() = default;
	~TaskQueue() override { stop(); }

	TaskQueue(const TaskQueue &)			= delete;
	TaskQueue &operator=(const TaskQueue &) = delete;

	// Enqueues a task for execution on the worker thread (FIFO order).
	void	   post(Task task)
	{
		{
			std::lock_guard<std::mutex> lock(mQueueMutex);
			mQueue.push(std::move(task));
		}
		triggerEvent();
	}

	// Number of tasks currently pending (not yet started).
	size_t pending() const
	{
		std::lock_guard<std::mutex> lock(mQueueMutex);
		return mQueue.size();
	}

	// Stops the worker thread. Any tasks still queued at the time of stop()
	// are dropped (not executed). Call drain-before-stop manually if needed.
	void stop() override
	{
		ThreadBase::stop();

		std::lock_guard<std::mutex> lock(mQueueMutex);
		std::queue<Task>			empty;
		std::swap(mQueue, empty);
	}

protected:
	void run() override
	{
		while (isRunning())
		{
			waitForEvent();

			for (;;)
			{
				Task task;
				{
					std::lock_guard<std::mutex> lock(mQueueMutex);
					if (mQueue.empty())
						break;

					task = std::move(mQueue.front());
					mQueue.pop();
				}

				// Executed outside the lock so post() isn't blocked while a task runs.
				task();
			}
		}
	}

private:
	mutable std::mutex mQueueMutex;
	std::queue<Task>   mQueue;
};
