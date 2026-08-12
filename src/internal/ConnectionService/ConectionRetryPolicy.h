/*
  ==============================================================================
	Module:         ConnectionRetryPolicy
	Description:    Encapsulates connection retry attempt bookkeeping
  ==============================================================================
*/

#pragma once


namespace netlink
{

class ConnectionRetryPolicy
{
public:
	explicit ConnectionRetryPolicy(int maxRetries = 3) : mMaxRetries(maxRetries) {}
	~ConnectionRetryPolicy() = default;

	void setMaxRetries(int maxRetries) { mMaxRetries = maxRetries; }

	void reset() { mAttempts = 0; }

	bool canRetry() const { return mAttempts < mMaxRetries; }

	// Records an attempt if one is still permitted by the configured limit.
	// Returns true if the attempt was recorded (i.e. retry should proceed),
	// false if the max-retries limit has already been reached.
	bool recordAttempt()
	{
		if (!canRetry())
			return false;

		++mAttempts;
		return true;
	}

	int attempts() const { return mAttempts; }
	int maxRetries() const { return mMaxRetries; }

private:
	int mAttempts{0};
	int mMaxRetries;
};

} // namespace netlink
