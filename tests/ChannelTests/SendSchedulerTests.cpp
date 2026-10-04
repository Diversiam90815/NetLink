#include <gtest/gtest.h>

#include <chrono>
#include <map>
#include <utility>
#include <vector>

#include "Channel/SendScheduler.h"
#include "TestIp.h"

using namespace netlink;
using namespace netlink::channel;
using namespace std::chrono_literals;


namespace ChannelTests
{

class SendSchedulerTest : public ::testing::Test
{
protected:
	using Result = SendScheduler::Result;
	using Sent	 = std::vector<std::pair<SendClass, int>>; // class and peer, in the order they were sent

	static SendScheduler::Peer peer(const int number) { return {ipv4("10.0.0.1"), static_cast<uint16_t>(number)}; }

	// The peer has that many datagrams to send in the class
	void					   give(SendScheduler &scheduler, const SendClass sendClass, const int number, const size_t datagrams)
	{
		supply[{sendClass, number}] = datagrams;
		scheduler.add(sendClass, peer(number));
	}

	// One run in which every peer sends what it was given
	Sent run(SendScheduler &scheduler)
	{
		Sent sent;

		scheduler.run(
			[&](const SendClass sendClass, const SendScheduler::Peer &from)
			{
				++asked;

				auto &left = supply[{sendClass, from.port}];
				if (left == 0)
					return Result::Empty;

				--left;
				sent.emplace_back(sendClass, from.port);
				return Result::Sent;
			});

		return sent;
	}

	static Sent turns(const SendClass sendClass, std::initializer_list<std::pair<int, size_t>> peers)
	{
		Sent sent;
		for (const auto &[number, count] : peers)
			sent.insert(sent.end(), count, {sendClass, number});
		return sent;
	}

	std::map<std::pair<SendClass, int>, size_t> supply;
	size_t										asked{0};
	SendScheduler::TimePoint					now = SendScheduler::Clock::now();
};


// ---------------------------------------------------------------------------
// Budget
// ---------------------------------------------------------------------------

TEST_F(SendSchedulerTest, StartsWithOneBurst)
{
	SendScheduler scheduler(80'000);
	EXPECT_EQ(scheduler.burst(), 160u) << "Two ticks of 1 ms";

	give(scheduler, SendClass::Application, 1, 1000);

	EXPECT_EQ(run(scheduler).size(), 160u);
	EXPECT_TRUE(scheduler.hasBacklog()) << "The rest waits for tokens";
	EXPECT_TRUE(run(scheduler).empty());
}


TEST_F(SendSchedulerTest, Burst_IsNeverSmallerThanSixteen)
{
	EXPECT_EQ(SendScheduler(1000).burst(), 16u);
	EXPECT_EQ(SendScheduler(8000).burst(), 16u);
	EXPECT_EQ(SendScheduler(20'000).burst(), 40u);
}


TEST_F(SendSchedulerTest, Refill_FollowsTheClock)
{
	SendScheduler scheduler(10'000); // 10 per ms, burst 20
	give(scheduler, SendClass::Application, 1, 1000);

	scheduler.refill(now);
	EXPECT_EQ(run(scheduler).size(), 20u);

	scheduler.refill(now += 1ms);
	EXPECT_EQ(run(scheduler).size(), 10u);

	scheduler.refill(now += 500us);
	EXPECT_EQ(run(scheduler).size(), 5u);

	scheduler.refill(now += 50ms);
	EXPECT_EQ(run(scheduler).size(), 20u) << "Time that was not used is not saved up beyond one burst";
}


TEST_F(SendSchedulerTest, LongRun_SendsAtTheConfiguredRate)
{
	for (const uint32_t rate : {1000u, 7000u, 80'000u})
	{
		SendScheduler scheduler(rate);
		give(scheduler, SendClass::Application, 1, 1'000'000);

		size_t sent = 0;
		for (int tick = 0; tick <= 1000; ++tick)
		{
			scheduler.refill(now += 1ms);
			sent += run(scheduler).size();
		}

		const auto afterTheFirstBurst = static_cast<double>(sent - scheduler.burst());
		EXPECT_NEAR(afterTheFirstBurst, rate, rate * 0.01) << "at " << rate << " datagrams per second";
	}
}


TEST_F(SendSchedulerTest, Spend_MayGoBelowZeroAndIsPaidBack)
{
	SendScheduler scheduler(10'000); // burst 20
	give(scheduler, SendClass::Application, 1, 1000);
	scheduler.refill(now);

	// Acknowledgements are sent without asking: 50 of them
	scheduler.spend(50);
	EXPECT_FALSE(scheduler.hasTokens());

	scheduler.refill(now += 2ms);
	EXPECT_TRUE(run(scheduler).empty()) << "20 - 50 + 20 tokens: still in debt";

	scheduler.refill(now += 2ms);
	EXPECT_EQ(run(scheduler).size(), 10u) << "What went out unasked is taken off the data that follows";
}


TEST_F(SendSchedulerTest, Unlimited_SendsEverythingAtOnce)
{
	SendScheduler scheduler; // no rate
	give(scheduler, SendClass::Application, 1, 5000);
	give(scheduler, SendClass::Unreliable, 2, 5000);

	scheduler.spend(1'000'000);

	EXPECT_EQ(run(scheduler).size(), 10'000u);
	EXPECT_FALSE(scheduler.hasBacklog());
}


// ---------------------------------------------------------------------------
// Who sends next
// ---------------------------------------------------------------------------

TEST_F(SendSchedulerTest, Classes_GoInOrderOfUrgency)
{
	SendScheduler scheduler;
	give(scheduler, SendClass::Application, 1, 2);
	give(scheduler, SendClass::Unreliable, 1, 2);
	give(scheduler, SendClass::Control, 1, 2);

	Sent expected = turns(SendClass::Control, {{1, 2}});
	expected.append_range(turns(SendClass::Unreliable, {{1, 2}}));
	expected.append_range(turns(SendClass::Application, {{1, 2}}));

	EXPECT_EQ(run(scheduler), expected);
}


TEST_F(SendSchedulerTest, Peers_TakeTurnsOfFourDatagrams)
{
	SendScheduler scheduler;
	for (const int number : {1, 2, 3})
		give(scheduler, SendClass::Application, number, 6);

	EXPECT_EQ(run(scheduler), turns(SendClass::Application, {{1, 4}, {2, 4}, {3, 4}, {1, 2}, {2, 2}, {3, 2}}));
}


TEST_F(SendSchedulerTest, Turn_ContinuesWhereTheBudgetEnded)
{
	SendScheduler scheduler(1000); // burst 16, one token per ms
	for (const int number : {1, 2, 3})
		give(scheduler, SendClass::Application, number, 100);

	scheduler.refill(now);
	scheduler.spend(10); // 6 tokens left

	EXPECT_EQ(run(scheduler), turns(SendClass::Application, {{1, 4}, {2, 2}}));

	scheduler.refill(now += 6ms);
	EXPECT_EQ(run(scheduler), turns(SendClass::Application, {{2, 2}, {3, 4}})) << "Peer 2 finishes its turn, then it is peer 3's: nobody is served twice in a row";
}


TEST_F(SendSchedulerTest, UnreliableData_LeavesAQuarterOfTheBudgetToReliableData)
{
	SendScheduler scheduler(80'000); // 160 tokens
	give(scheduler, SendClass::Unreliable, 1, 1000);
	give(scheduler, SendClass::Application, 2, 1000);

	Sent expected = turns(SendClass::Unreliable, {{1, 120}});
	expected.append_range(turns(SendClass::Application, {{2, 40}}));
	EXPECT_EQ(run(scheduler), expected) << "Unreliable data comes first, but must not starve retransmissions";
}


TEST_F(SendSchedulerTest, UnreliableData_UsesTheWholeBudgetWhenNothingElseWaits)
{
	SendScheduler scheduler(80'000);
	give(scheduler, SendClass::Unreliable, 1, 1000);

	EXPECT_EQ(run(scheduler).size(), 160u);
}


TEST_F(SendSchedulerTest, EmptyPeer_LeavesTheRing)
{
	SendScheduler scheduler;
	give(scheduler, SendClass::Application, 1, 1);
	give(scheduler, SendClass::Application, 2, 9);

	EXPECT_EQ(run(scheduler).size(), 10u);
	EXPECT_FALSE(scheduler.hasBacklog());

	asked = 0;
	run(scheduler);
	EXPECT_EQ(asked, 0u) << "Nobody is asked again before it is added again";
}


TEST_F(SendSchedulerTest, Blocked_EndsTheRunWithoutSpending)
{
	SendScheduler scheduler(80'000);
	scheduler.add(SendClass::Control, peer(1));
	scheduler.add(SendClass::Application, peer(2));

	size_t calls = 0;
	scheduler.run(
		[&](SendClass, const SendScheduler::Peer &)
		{
			++calls;
			return Result::Blocked;
		});

	EXPECT_EQ(calls, 1u) << "The socket takes nothing: nobody else is asked";
	EXPECT_EQ(scheduler.tokens(), 160.0);
	EXPECT_TRUE(scheduler.hasBacklog()) << "Both are still waiting";
}


TEST_F(SendSchedulerTest, Lost_CountsAsSentAndEndsTheTurn)
{
	SendScheduler scheduler(1000); // 16 tokens
	scheduler.add(SendClass::Application, peer(1));
	scheduler.add(SendClass::Application, peer(2));

	std::vector<int> order;
	scheduler.run(
		[&](SendClass, const SendScheduler::Peer &from)
		{
			order.push_back(from.port);
			return from.port == 1 ? Result::Lost : Result::Sent;
		});

	EXPECT_EQ(order, (std::vector<int>{1, 2, 2, 2, 2, 1, 2, 2, 2, 2, 1, 2, 2, 2, 2, 1})) << "A peer that cannot be reached gets one try per round";
	EXPECT_FALSE(scheduler.hasTokens()) << "Its tries are paid for like everything else";
}


TEST_F(SendSchedulerTest, Remove_ForgetsThePeerInEveryClass)
{
	SendScheduler scheduler;
	for (const int number : {1, 2, 3})
	{
		give(scheduler, SendClass::Control, number, 1);
		give(scheduler, SendClass::Application, number, 5);
	}

	scheduler.remove(peer(2));

	Sent expected = turns(SendClass::Control, {{1, 1}, {3, 1}});
	expected.append_range(turns(SendClass::Application, {{1, 4}, {3, 4}, {1, 1}, {3, 1}}));
	EXPECT_EQ(run(scheduler), expected);

	scheduler.add(SendClass::Application, peer(1));
	scheduler.clear();
	EXPECT_FALSE(scheduler.hasBacklog());
}

} // namespace ChannelTests
