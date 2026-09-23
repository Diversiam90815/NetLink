/*
  ==============================================================================
	Module:         TimeoutService
	Description:    Manager for handling multiple concurrent timeouts
  ==============================================================================
*/

#include "TimeoutService.h"

#include <algorithm>


TimeoutService::~TimeoutService()
{
	{
		std::lock_guard<std::mutex> lock(mMutex);
		mStopping = true;
		mActiveTimeouts.clear();
	}
	mWakeUp.notify_all();

	if (mWorker.joinable())
	{
		if (mWorker.get_id() == std::this_thread::get_id())
			mWorker.detach(); // destroyed from inside a callback; the loop exits without touching members again
		else
			mWorker.join();
	}
}


void TimeoutService::startTimeout(const TimeoutKey &key, int timeoutMS, TimeoutCallback callback)
{
	{
		std::lock_guard<std::mutex> lock(mMutex);

		if (mStopping)
			return;

		// Replacing an entry also discards a pending (not yet running) callback for the same key
		mActiveTimeouts[key] = Entry{Clock::now() + std::chrono::milliseconds(std::max(timeoutMS, 0)), std::move(callback)};

		if (!mWorker.joinable())
			mWorker = std::thread(&TimeoutService::run, this);
	}

	mWakeUp.notify_all();
}


bool TimeoutService::cancelTimeout(const TimeoutKey &key)
{
	std::unique_lock<std::mutex> lock(mMutex);
	return cancelIf([&key](const TimeoutKey &candidate) { return candidate == key; }, lock) > 0;
}


int TimeoutService::cancelCategory(const std::string &category)
{
	std::unique_lock<std::mutex> lock(mMutex);
	return cancelIf([&category](const TimeoutKey &candidate) { return candidate.category == category; }, lock);
}


int TimeoutService::cancelByIdentifier(const std::string &identifier)
{
	std::unique_lock<std::mutex> lock(mMutex);
	return cancelIf([&identifier](const TimeoutKey &candidate) { return candidate.identifier == identifier; }, lock);
}


void TimeoutService::cancelAll()
{
	std::unique_lock<std::mutex> lock(mMutex);
	cancelIf([](const TimeoutKey &) { return true; }, lock);
}


bool TimeoutService::isActive(const TimeoutKey &key) const
{
	std::lock_guard<std::mutex> lock(mMutex);
	return mActiveTimeouts.contains(key);
}


size_t TimeoutService::activeCount() const
{
	std::lock_guard<std::mutex> lock(mMutex);
	return mActiveTimeouts.size();
}


int TimeoutService::cancelIf(const std::function<bool(const TimeoutKey &)> &matches, std::unique_lock<std::mutex> &lock)
{
	const size_t removed = std::erase_if(mActiveTimeouts, [&matches](const auto &entry) { return matches(entry.first); });

	// A matching callback may be executing right now: wait for it, unless we are that callback
	const bool	 onWorker = mWorker.joinable() && mWorker.get_id() == std::this_thread::get_id();

	if (!onWorker)
		mCallbackDone.wait(lock, [&] { return !mRunningKey.has_value() || !matches(*mRunningKey); });

	return static_cast<int>(removed);
}


void TimeoutService::run()
{
	std::unique_lock<std::mutex> lock(mMutex);

	while (!mStopping)
	{
		if (mActiveTimeouts.empty())
		{
			mWakeUp.wait(lock, [this] { return mStopping || !mActiveTimeouts.empty(); });
			continue;
		}

		auto next = std::ranges::min_element(mActiveTimeouts, {}, [](const auto &entry) { return entry.second.deadline; });

		if (Clock::now() < next->second.deadline)
		{
			mWakeUp.wait_until(lock, next->second.deadline); // re-evaluated after new/cancelled timeouts
			continue;
		}

		TimeoutKey		key		 = next->first;
		TimeoutCallback callback = std::move(next->second.callback);
		mActiveTimeouts.erase(next);

		mRunningKey = key;
		lock.unlock();

		if (callback)
			callback(key);

		lock.lock();
		mRunningKey.reset();
		mCallbackDone.notify_all();
	}
}
