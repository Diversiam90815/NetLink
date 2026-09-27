#include <gtest/gtest.h>

#include <string>

#include "Channel/Queue/BoundedQueue.h"

using namespace netlink;
using namespace netlink::channel;


namespace ChannelTests
{

TEST(BoundedQueue, IsFifo)
{
	BoundedQueue<int> queue(4);

	for (int i = 1; i <= 4; ++i)
		EXPECT_EQ(queue.push(i), PushResult::Accepted);

	EXPECT_TRUE(queue.full());

	for (int i = 1; i <= 4; ++i)
		EXPECT_EQ(queue.pop(), i);

	EXPECT_TRUE(queue.empty());
	EXPECT_FALSE(queue.pop().has_value());
}


TEST(BoundedQueue, WrapsAroundTheRingBuffer)
{
	BoundedQueue<int> queue(3);

	for (int round = 0; round < 10; ++round)
	{
		ASSERT_EQ(queue.push(round * 2), PushResult::Accepted);
		ASSERT_EQ(queue.push(round * 2 + 1), PushResult::Accepted);
		EXPECT_EQ(queue.pop(), round * 2);
		EXPECT_EQ(queue.pop(), round * 2 + 1);
	}

	EXPECT_TRUE(queue.empty());
}


TEST(BoundedQueue, IndexedAccessStartsAtTheOldest)
{
	BoundedQueue<int> queue(3);
	queue.push(1);
	queue.push(2);
	queue.pop();
	queue.push(3);
	queue.push(4);

	EXPECT_EQ(queue.front(), 2);
	EXPECT_EQ(queue.at(0), 2);
	EXPECT_EQ(queue.at(1), 3);
	EXPECT_EQ(queue.at(2), 4);
}


TEST(BoundedQueue, DropNewest_RejectsWhenFull)
{
	BoundedQueue<int> queue(2, OverflowPolicy::DropNewest);
	queue.push(1);
	queue.push(2);

	EXPECT_EQ(queue.push(3), PushResult::Rejected);
	EXPECT_EQ(queue.size(), 2u);
	EXPECT_EQ(queue.pop(), 1) << "Queued items are untouched";
	EXPECT_EQ(queue.pop(), 2);
}


TEST(BoundedQueue, DropOldest_EvictsAndHandsOutTheOldest)
{
	BoundedQueue<std::string> queue(2, OverflowPolicy::DropOldest);
	queue.push("a");
	queue.push("b");

	std::optional<std::string> evicted;
	EXPECT_EQ(queue.push("c", &evicted), PushResult::EvictedOldest);
	EXPECT_EQ(evicted, "a");

	EXPECT_EQ(queue.size(), 2u);
	EXPECT_EQ(queue.pop(), "b");
	EXPECT_EQ(queue.pop(), "c");
}


TEST(BoundedQueue, ClearEmptiesTheQueue)
{
	BoundedQueue<int> queue(3);
	queue.push(1);
	queue.push(2);
	queue.clear();

	EXPECT_TRUE(queue.empty());
	EXPECT_EQ(queue.push(5), PushResult::Accepted);
	EXPECT_EQ(queue.pop(), 5);
}


TEST(BoundedQueue, ZeroCapacityStillHoldsOneItem)
{
	BoundedQueue<int> queue(0);

	EXPECT_EQ(queue.capacity(), 1u);
	EXPECT_EQ(queue.push(1), PushResult::Accepted);
}

} // namespace ChannelTests
