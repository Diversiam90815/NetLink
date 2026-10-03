/*
  ==============================================================================
	Module:         TimeoutService
	Description:    Manager for handling multiple concurrent timeouts
  ==============================================================================
*/

#include "TimeoutService.h"

#include <algorithm>


TimeoutService::TimeoutService(std::unique_ptr<netlink::IDeadlineTimer> timer) : mTimer(timer ? std::move(timer) : netlink::makeDeadlineTimer()) {}


TimeoutService::~TimeoutService()
{
	{
		std::lock_guard<std::mutex> lock(mMutex);
		mStopping = true;
		mActiveTimeouts.clear();
		mDeadlines.clear();
	}
	mTimer->wake();

	if (mWorker.joinable())
	{
		if (mWorker.get_id() == std::this_thread::get_id())
			mWorker.detach(); // destroyed from inside a callback; the loop exits without touching members again
		else
			mWorker.join();
	}
}


void TimeoutService::startTimeout(const TimeoutKey &key, const int timeoutMS, TimeoutCallback callback)
{
	{
		std::lock_guard<std::mutex> lock(mMutex);

		if (mStopping)
			return;

		const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(timeoutMS, 0));

		// Replacing an entry also discards a pending (not yet running) callback for the same key
		if (const auto it = mActiveTimeouts.find(key); it != mActiveTimeouts.end())
		{
			mDeadlines.erase({it->second.deadline, key});
			it->second = Entry{.deadline = deadline, .callback = std::move(callback)};
		}
		else
		{
			mActiveTimeouts.emplace(key, Entry{.deadline = deadline, .callback = std::move(callback)});
		}

		mDeadlines.emplace(deadline, key);

		if (!mWorker.joinable())
			mWorker = std::thread(&TimeoutService::run, this);
	}

	mTimer->wake();
}


bool TimeoutService::cancelTimeout(const TimeoutKey &key)
{
	std::unique_lock<std::mutex> lock(mMutex);

	const auto					 it		 = mActiveTimeouts.find(key);
	const bool					 removed = it != mActiveTimeouts.end();

	if (removed)
		erase(it);

	waitForRunningCallback([&key](const TimeoutKey &candidate) { return candidate == key; }, lock);
	return removed;
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


TimeoutService::Timeouts::iterator TimeoutService::erase(const Timeouts::iterator it)
{
	mDeadlines.erase({it->second.deadline, it->first});
	return mActiveTimeouts.erase(it);
}


int TimeoutService::cancelIf(const std::function<bool(const TimeoutKey &)> &matches, std::unique_lock<std::mutex> &lock)
{
	int removed = 0;

	for (auto it = mActiveTimeouts.begin(); it != mActiveTimeouts.end();)
	{
		if (matches(it->first))
		{
			it = erase(it);
			++removed;
		}
		else
			++it;
	}

	waitForRunningCallback(matches, lock);
	return removed;
}


void TimeoutService::waitForRunningCallback(const std::function<bool(const TimeoutKey &)> &matches, std::unique_lock<std::mutex> &lock)
{
	// A matching callback may be executing right now: wait for it, unless we are that callback
	if (const bool onWorker = mWorker.joinable() && mWorker.get_id() == std::this_thread::get_id(); !onWorker)
		mCallbackDone.wait(lock, [&] { return !mRunningKey.has_value() || !matches(*mRunningKey); });
}


void TimeoutService::run()
{
	std::unique_lock<std::mutex> lock(mMutex);

	while (!mStopping)
	{
		// A copy: the entry may be cancelled while the lock is released during the wait
		if (const auto deadline = mDeadlines.empty() ? netlink::IDeadlineTimer::Never : mDeadlines.begin()->first; mDeadlines.empty() || Clock::now() < deadline)
		{
			// A wake-up between unlocking and waiting is not lost: it ends this wait. Re-evaluated after new timeouts.
			lock.unlock();
			mTimer->waitUntil(deadline);
			lock.lock();
			continue;
		}

		const auto		it		 = mActiveTimeouts.find(mDeadlines.begin()->second);
		TimeoutKey		key		 = it->first;
		TimeoutCallback callback = std::move(it->second.callback);
		erase(it);

		mRunningKey = key;
		lock.unlock();

		if (callback)
			callback(key);

		lock.lock();
		mRunningKey.reset();
		mCallbackDone.notify_all();
	}
}
