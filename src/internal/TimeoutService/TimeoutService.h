/*
  ==============================================================================
	Module:         TimeoutService
	Description:    Manager for handling multiple concurrent timeouts
  ==============================================================================
*/

#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>


/**
 * @brief	Identifies a specific timeout instance
 */
struct TimeoutKey
{
	std::string category;	// e.g. "version_request", "invitation",..
	std::string identifier; // remote computer

	bool		operator<(const TimeoutKey &other) const
	{
		if (category != other.category)
			return category < other.category;

		return identifier < other.identifier;
	}

	bool		operator==(const TimeoutKey &other) const = default;

	std::string toString() const { return category + ": " + identifier; }
};


/**
 * @brief	Callback signature for timeout events
 * @param	key The timeout that expired
 */
using TimeoutCallback = std::function<void(const TimeoutKey &key)>;


/**
 * @brief	Runs any number of timeouts on one worker thread.
 */
class TimeoutService
{
public:
	TimeoutService() = default;
	~TimeoutService();

	TimeoutService(const TimeoutService &)			  = delete;
	TimeoutService &operator=(const TimeoutService &) = delete;

	/**
	 * @brief	Start a new timeout, replacing an active one with the same key
	 * @param	key Unique identifier for this timeout
	 * @param	timeoutMs Timeout duration in milliseconds
	 * @param	callback Function to call when timeout expires
	 */
	void   startTimeout(const TimeoutKey &key, int timeoutMS, TimeoutCallback callback);

	/**
	 * @brief	Cancel a specific timeout
	 * @return	true if timeout was found and cancelled
	 */
	bool   cancelTimeout(const TimeoutKey &key);

	/**
	 * @brief	Cancel all timeouts matching a category
	 * @return	Number of timeouts cancelled
	 */
	int	   cancelCategory(const std::string &category);

	/**
	 * @brief	Cancel all timeouts for a specific remote
	 * @return	Number of timeouts cancelled
	 */
	int	   cancelByIdentifier(const std::string &identifier);

	/**
	 * @brief	Cancel all active timeouts
	 */
	void   cancelAll();

	/**
	 * @brief	Check if a specific timeout is active (pending, not yet fired)
	 */
	bool   isActive(const TimeoutKey &key) const;

	/**
	 * @brief	Get count of active timeouts
	 */
	size_t activeCount() const;

private:
	using Clock = std::chrono::steady_clock;

	struct Entry
	{
		Clock::time_point deadline;
		TimeoutCallback	  callback;
	};

	void												run();
	int													cancelIf(const std::function<bool(const TimeoutKey &)> &matches, std::unique_lock<std::mutex> &lock);

	mutable std::mutex									mMutex;
	std::condition_variable								mWakeUp;	  // new timeout / stop
	std::condition_variable								mCallbackDone; // a callback finished
	std::map<TimeoutKey, Entry>							mActiveTimeouts;
	std::optional<TimeoutKey>							mRunningKey;  // key whose callback is executing right now

	std::thread											mWorker;
	bool												mStopping{false};
};
