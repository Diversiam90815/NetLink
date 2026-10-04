#include <gtest/gtest.h>

#include "TestIp.h"
#include "Channel/Heartbeat/HeartbeatService.h"

using namespace netlink;
using namespace netlink::channel;
using namespace std::chrono_literals;


namespace ChannelTests
{

class HeartbeatServiceTest : public ::testing::Test
{
protected:
	// Asks after 1 s without a word, gives up after 5 s
	HeartbeatService			service{LinkTimings{}};
	HeartbeatService::TimePoint t0 = HeartbeatService::Clock::now();
	net::SocketAddress			peer{ipv4("10.0.0.2"), 50000};
};


TEST_F(HeartbeatServiceTest, UnwatchedPeersProduceNothing)
{
	auto tick = service.tick(t0 + 10s);

	EXPECT_TRUE(tick.pingsDue.empty());
	EXPECT_TRUE(tick.silentPeers.empty());
	EXPECT_FALSE(service.nextDeadline().has_value());
}


TEST_F(HeartbeatServiceTest, PeerIsAskedAfterOneSilentInterval)
{
	service.watch(peer, t0);

	EXPECT_TRUE(service.tick(t0 + 999ms).pingsDue.empty());

	auto tick = service.tick(t0 + 1000ms);
	ASSERT_EQ(tick.pingsDue.size(), 1u);
	EXPECT_EQ(tick.pingsDue.front(), peer);

	EXPECT_TRUE(service.tick(t0 + 1500ms).pingsDue.empty()) << "It gets an interval to answer before it is asked again";
	EXPECT_EQ(service.tick(t0 + 2000ms).pingsDue.size(), 1u) << "No answer: asked again";
}


TEST_F(HeartbeatServiceTest, IncomingTrafficPostponesThePing)
{
	service.watch(peer, t0);
	service.onReceived(peer, t0 + 800ms);

	EXPECT_TRUE(service.tick(t0 + 1200ms).pingsDue.empty()) << "A peer that was just heard from does not need to be asked";
	EXPECT_EQ(service.tick(t0 + 1800ms).pingsDue.size(), 1u);
}


TEST_F(HeartbeatServiceTest, SilentPeerIsReportedOnceAndUnwatched)
{
	service.watch(peer, t0);

	EXPECT_TRUE(service.tick(t0 + 4999ms).silentPeers.empty());

	auto tick = service.tick(t0 + 5000ms);
	ASSERT_EQ(tick.silentPeers.size(), 1u);
	EXPECT_EQ(tick.silentPeers.front(), peer);
	EXPECT_FALSE(service.isWatched(peer));

	EXPECT_TRUE(service.tick(t0 + 10s).silentPeers.empty()) << "Reported only once";
}


TEST_F(HeartbeatServiceTest, IncomingTrafficKeepsThePeerAlive)
{
	service.watch(peer, t0);

	for (auto t = 1s; t <= 20s; t += 1s)
	{
		service.onReceived(peer, t0 + t);
		EXPECT_TRUE(service.tick(t0 + t + 500ms).silentPeers.empty());
	}
}


TEST_F(HeartbeatServiceTest, NextDeadlineIsTheEarlierOfPingAndSilence)
{
	service.watch(peer, t0);
	ASSERT_TRUE(service.nextDeadline().has_value());
	EXPECT_EQ(*service.nextDeadline(), t0 + 1000ms);

	// Asked four times without an answer: the last question would come after the peer is given up on
	for (auto t = 1s; t <= 4s; t += 1s)
		service.tick(t0 + t);
	service.tick(t0 + 4500ms);

	EXPECT_EQ(*service.nextDeadline(), t0 + 5000ms) << "The silence timeout is nearer than the next ping";
}


TEST_F(HeartbeatServiceTest, Timings_CanBeShortened)
{
	LinkTimings timings;
	timings.keepAlive	= 50ms;
	timings.peerTimeout = 400ms;
	service.setTimings(timings);

	service.watch(peer, t0);

	EXPECT_EQ(service.tick(t0 + 50ms).pingsDue.size(), 1u);
	EXPECT_EQ(service.tick(t0 + 400ms).silentPeers.size(), 1u);
}


TEST_F(HeartbeatServiceTest, UnwatchStopsSupervision)
{
	service.watch(peer, t0);
	service.unwatch(peer);

	EXPECT_FALSE(service.isWatched(peer));
	EXPECT_TRUE(service.tick(t0 + 10s).silentPeers.empty());
}

} // namespace ChannelTests
