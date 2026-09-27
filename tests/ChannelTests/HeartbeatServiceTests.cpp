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
	HeartbeatService			service{HeartbeatConfig{1000ms, 5000ms}};
	HeartbeatService::TimePoint t0 = HeartbeatService::Clock::now();
	net::SocketAddress			peer{ipv4("10.0.0.2"), 50000};
};


TEST_F(HeartbeatServiceTest, UnwatchedPeersProduceNothing)
{
	auto tick = service.tick(t0 + 10s);

	EXPECT_TRUE(tick.heartbeatsDue.empty());
	EXPECT_TRUE(tick.silentPeers.empty());
	EXPECT_FALSE(service.nextDeadline().has_value());
}


TEST_F(HeartbeatServiceTest, HeartbeatIsDueAfterOneIdleInterval)
{
	service.watch(peer, t0);

	EXPECT_TRUE(service.tick(t0 + 999ms).heartbeatsDue.empty());

	auto tick = service.tick(t0 + 1000ms);
	ASSERT_EQ(tick.heartbeatsDue.size(), 1u);
	EXPECT_EQ(tick.heartbeatsDue.front(), peer);

	EXPECT_TRUE(service.tick(t0 + 1500ms).heartbeatsDue.empty()) << "The heartbeat itself counts as sent traffic";
}


TEST_F(HeartbeatServiceTest, OutgoingTrafficPostponesTheHeartbeat)
{
	service.watch(peer, t0);
	service.onSent(peer, t0 + 800ms);

	EXPECT_TRUE(service.tick(t0 + 1200ms).heartbeatsDue.empty()) << "Only an idle link needs a heartbeat";
	EXPECT_EQ(service.tick(t0 + 1800ms).heartbeatsDue.size(), 1u);
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


TEST_F(HeartbeatServiceTest, NextDeadlineIsTheEarlierOfHeartbeatAndSilence)
{
	service.watch(peer, t0);
	ASSERT_TRUE(service.nextDeadline().has_value());
	EXPECT_EQ(*service.nextDeadline(), t0 + 1000ms);

	service.onSent(peer, t0 + 4500ms);
	EXPECT_EQ(*service.nextDeadline(), t0 + 5000ms) << "The silence timeout is nearer than the next heartbeat";
}


TEST_F(HeartbeatServiceTest, UnwatchStopsSupervision)
{
	service.watch(peer, t0);
	service.unwatch(peer);

	EXPECT_FALSE(service.isWatched(peer));
	EXPECT_TRUE(service.tick(t0 + 10s).silentPeers.empty());
}

} // namespace ChannelTests
