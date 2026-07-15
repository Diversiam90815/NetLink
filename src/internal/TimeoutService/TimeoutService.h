/*
  ==============================================================================
	Module:         TimeoutService
	Description:    Manager for handling multiple concurrent timeouts
  ==============================================================================
*/

#pragma once

#include <functional>
#include <map>
#include <future>
#include <chrono>
#include <mutex>
#include <atomic>
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

	std::string toString() const { return category + ": " + identifier; }
};


/**
 * @brief	Callback signature for timeout events
 * @param	key The timeout that expired
 */
using TimeoutCallback = std::function<void(const TimeoutKey &key)>;


/**
 * @brief	Internal tracking of an active timeout
 */
struct ActiveTimeout
{
	TimeoutKey							  key;
	std::chrono::steady_clock::time_point deadline;
	std::atomic<bool>					  cancelled{false};
	std::future<void>					  future;
};



class TimeoutService
{
public:
	TimeoutService() = default;
	~TimeoutService() { cancelAll(); }

	/**
	 * @brief	Start a new timeout
	 * @param	key Unique identifier for this timeout
	 * @param	timeoutMs Timeout duration in milliseconds
	 * @param	callback Function to call when timeout expires
	 */
	void   startTimeout(const TimeoutKey &key, int timeoutMS, TimeoutCallback callback);

	/**
	 * @brief	Cancel a specific timeout
	 * @param	key The timeout to cancel
	 * @return	true if timeout was found and cancelled
	 */
	bool   cancelTimeout(const TimeoutKey &key);

	/**
	 * @brief	Cancel all timeouts matching a category
	 * @param	category The category to cancel
	 * @return	Number of timeouts cancelled
	 */
	int	   cancelCategory(const std::string &category);

	/**
	 * @brief	Cancel all timeouts for a specific remote
	 * @param	identifier The identifier (e.g., computer name) to cancel
	 * @return	Number of timeouts cancelled
	 */
	int	   cancelByIdentifier(const std::string &identifier);

	/**
	 * @brief	Cancel all active timeouts
	 */
	void   cancelAll();

	/**
	 * @brief	Check if a specific timeout is active
	 */
	bool   isActive(const TimeoutKey &key) const;

	/**
	 * @brief	Get count of active timeouts
	 */
	size_t activeCount() const;

private:
	void												 cleanupCompleted();

	mutable std::mutex									 mMutex;
	std::map<TimeoutKey, std::shared_ptr<ActiveTimeout>> mActiveTimeouts;
};
