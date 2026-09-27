/*
  ==============================================================================
	Module:         RttEstimator
	Description:    Round-trip time estimation and retransmission timeout
  ==============================================================================
*/

#pragma once

#include <algorithm>
#include <chrono>


namespace netlink::channel
{

class RttEstimator
{
public:
	using Duration = std::chrono::microseconds;

	RttEstimator(const std::chrono::milliseconds initialRto, const std::chrono::milliseconds minRto, const std::chrono::milliseconds maxRto)
		: mInitialRto(initialRto), mMinRto(minRto), mMaxRto(maxRto), mRto(clamp(initialRto))
	{
	}

	void addSample(const Duration rtt)
	{
		if (rtt.count() < 0)
			return;

		if (!mHasSample)
		{
			mSrtt	   = rtt;
			mRttVar	   = rtt / 2;
			mHasSample = true;
		}
		else
		{
			const Duration delta = mSrtt > rtt ? mSrtt - rtt : rtt - mSrtt;
			mRttVar				 = (mRttVar * 3 + delta) / 4;
			mSrtt				 = (mSrtt * 7 + rtt) / 8;
		}

		// RFC 6298: RTO = SRTT + max(G, 4 * RTTVAR), with the clock granularity G taken as 1 ms
		mRto = clamp(mSrtt + std::max<Duration>(std::chrono::milliseconds{1}, mRttVar * 4));
	}

	void reset()
	{
		mHasSample = false;
		mSrtt	   = {};
		mRttVar	   = {};
		mRto	   = clamp(mInitialRto);
	}

	// Timeout for the given retransmission attempt (0 = first transmission): doubles per attempt, capped at maxRto
	Duration timeoutFor(const int attempt) const
	{
		Duration timeout = mRto;
		for (int i = 0; i < attempt && timeout < mMaxRto; ++i)
			timeout *= 2;

		return clamp(timeout);
	}

	Duration rto() const { return mRto; }
	Duration srtt() const { return mSrtt; }
	Duration rttVar() const { return mRttVar; }
	bool	 hasSample() const { return mHasSample; }

private:
	Duration clamp(const Duration value) const { return std::clamp<Duration>(value, mMinRto, mMaxRto); }

	Duration mInitialRto;
	Duration mMinRto;
	Duration mMaxRto;

	Duration mRto;
	Duration mSrtt{};
	Duration mRttVar{};
	bool	 mHasSample{false};
};

} // namespace netlink::channel
