#include <gtest/gtest.h>

#include "Channel/Reliability/RttEstimator.h"

using namespace netlink::channel;
using namespace std::chrono_literals;


namespace ChannelTests
{

TEST(RttEstimator, StartsWithTheInitialRto)
{
	RttEstimator rtt(100ms, 20ms, 1000ms);

	EXPECT_FALSE(rtt.hasSample());
	EXPECT_EQ(rtt.rto(), 100ms);
}


TEST(RttEstimator, FirstSampleFollowsRfc6298)
{
	RttEstimator rtt(100ms, 1ms, 1000ms);
	rtt.addSample(40ms);

	EXPECT_EQ(rtt.srtt(), 40ms);
	EXPECT_EQ(rtt.rttVar(), 20ms);
	EXPECT_EQ(rtt.rto(), 40ms + 4 * 20ms) << "RTO = SRTT + 4 * RTTVAR";
}


TEST(RttEstimator, SmoothsFurtherSamples)
{
	RttEstimator rtt(100ms, 1ms, 1000ms);
	rtt.addSample(40ms);
	rtt.addSample(80ms);

	// RTTVAR = 3/4 * 20 + 1/4 * |40 - 80| = 25, SRTT = 7/8 * 40 + 1/8 * 80 = 45
	EXPECT_EQ(rtt.rttVar(), 25ms);
	EXPECT_EQ(rtt.srtt(), 45ms);
	EXPECT_EQ(rtt.rto(), 145ms);
}


TEST(RttEstimator, RtoIsClamped)
{
	RttEstimator fast(100ms, 20ms, 1000ms);
	fast.addSample(100us);
	EXPECT_EQ(fast.rto(), 20ms) << "LAN round trips must not produce a sub-minimum timeout";

	RttEstimator slow(100ms, 20ms, 1000ms);
	slow.addSample(5s);
	EXPECT_EQ(slow.rto(), 1000ms);
}


TEST(RttEstimator, BackoffDoublesUpToTheMaximum)
{
	RttEstimator rtt(100ms, 20ms, 1000ms);

	EXPECT_EQ(rtt.timeoutFor(0), 100ms);
	EXPECT_EQ(rtt.timeoutFor(1), 200ms);
	EXPECT_EQ(rtt.timeoutFor(2), 400ms);
	EXPECT_EQ(rtt.timeoutFor(3), 800ms);
	EXPECT_EQ(rtt.timeoutFor(4), 1000ms);
	EXPECT_EQ(rtt.timeoutFor(30), 1000ms) << "Must not overflow for many attempts";
}


TEST(RttEstimator, ResetRestoresTheInitialState)
{
	RttEstimator rtt(100ms, 20ms, 1000ms);
	rtt.addSample(500ms);
	rtt.reset();

	EXPECT_FALSE(rtt.hasSample());
	EXPECT_EQ(rtt.rto(), 100ms);
}

} // namespace ChannelTests
