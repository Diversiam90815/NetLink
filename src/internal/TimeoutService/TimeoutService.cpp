/*
  ==============================================================================
	Module:         TimeoutService
	Description:    Manager for handling multiple concurrent timeouts
  ==============================================================================
*/

#include "TimeoutService.h"


void TimeoutService::startTimeout(const TimeoutKey &key, int timeoutMS, TimeoutCallback callback)
{
	// Cancel any existing timeout with same key
	cancelTimeout(key);

	auto timeout	  = std::make_shared<ActiveTimeout>();
	timeout->key	  = key;
	timeout->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMS);

	// Capture shared_ptr to ensure timeout struct lives until async completes
	timeout->future	  = std::async(std::launch::async,
								   [timeout, callback, timeoutMS]()
								   {
									   constexpr int checkIntervalMS = 50;
									   int			 elapsed		 = 0;

									   while (elapsed < timeoutMS)
									   {
										   if (timeout->cancelled.load())
											   return;

										   std::this_thread::sleep_for(std::chrono::milliseconds(checkIntervalMS));
										   elapsed += checkIntervalMS;
									   }

									   // Final check before firing callback
									   if (!timeout->cancelled.load() && callback)
										   callback(timeout->key);
								   });

	{
		std::lock_guard<std::mutex> lock(mMutex);
		cleanupCompleted();
		mActiveTimeouts[key] = timeout;
	}
}


bool TimeoutService::cancelTimeout(const TimeoutKey &key)
{
	std::shared_ptr<ActiveTimeout> timeout;

	{
		std::lock_guard<std::mutex> lock(mMutex);
		auto						it = mActiveTimeouts.find(key);

		if (it == mActiveTimeouts.end())
			return false;

		timeout = it->second;
		mActiveTimeouts.erase(it);
	}

	// Cancel outside lock to avoid deadlock
	timeout->cancelled.store(true);
	if (timeout->future.valid())
		timeout->future.wait();

	return true;
}


int TimeoutService::cancelCategory(const std::string &category)
{
	std::vector<std::shared_ptr<ActiveTimeout>> toCancel;

	{
		std::lock_guard<std::mutex> lock(mMutex);
		for (auto it = mActiveTimeouts.begin(); it != mActiveTimeouts.end();)
		{
			if (it->first.category == category)
			{
				toCancel.push_back(it->second);
				it = mActiveTimeouts.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	// Cancel outside lock
	for (auto &timeout : toCancel)
	{
		timeout->cancelled.store(true);
		if (timeout->future.valid())
			timeout->future.wait();
	}

	return static_cast<int>(toCancel.size());
}


int TimeoutService::cancelByIdentifier(const std::string &identifier)
{
	std::vector<std::shared_ptr<ActiveTimeout>> toCancel;

	{
		std::lock_guard<std::mutex> lock(mMutex);
		for (auto it = mActiveTimeouts.begin(); it != mActiveTimeouts.end();)
		{
			if (it->first.identifier == identifier)
			{
				toCancel.push_back(it->second);
				it = mActiveTimeouts.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	// Cancel outside lock
	for (auto &timeout : toCancel)
	{
		timeout->cancelled.store(true);
		if (timeout->future.valid())
			timeout->future.wait();
	}

	return static_cast<int>(toCancel.size());
}


void TimeoutService::cancelAll()
{
	std::map<TimeoutKey, std::shared_ptr<ActiveTimeout>> timeouts;

	{
		std::lock_guard<std::mutex> lock(mMutex);
		timeouts = std::move(mActiveTimeouts);
		mActiveTimeouts.clear();
	}

	// Cancel outside lock
	for (auto &[key, timeout] : timeouts)
	{
		timeout->cancelled.store(true);
		if (timeout->future.valid())
			timeout->future.wait();
	}
}


bool TimeoutService::isActive(const TimeoutKey &key) const
{
	std::lock_guard<std::mutex> lock(mMutex);
	return mActiveTimeouts.find(key) != mActiveTimeouts.end();
}


size_t TimeoutService::activeCount() const
{
	std::lock_guard<std::mutex> lock(mMutex);
	return mActiveTimeouts.size();
}


void TimeoutService::cleanupCompleted()
{
	// Called while holding lock, we remove finished timeouts
	for (auto it = mActiveTimeouts.begin(); it != mActiveTimeouts.end();)
	{
		if (it->second->future.valid() && it->second->future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
			it = mActiveTimeouts.erase(it);

		else
			++it;
	}
}
