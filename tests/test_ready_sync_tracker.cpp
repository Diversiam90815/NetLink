#include <gtest/gtest.h>

#include "ConnectionService/ReadySyncTracker.h"

using namespace netlink;


TEST(ReadySyncTracker, DefaultConstructed_NeitherSideReady)
{
	ReadySyncTracker tracker;

	EXPECT_FALSE(tracker.isLocalReady());
	EXPECT_FALSE(tracker.isRemoteReady());
	EXPECT_FALSE(tracker.bothReady());
}


TEST(ReadySyncTracker, SetLocalReady_OnlyAffectsLocal)
{
	ReadySyncTracker tracker;
	tracker.setLocalReady();

	EXPECT_TRUE(tracker.isLocalReady());
	EXPECT_FALSE(tracker.isRemoteReady());
	EXPECT_FALSE(tracker.bothReady()) << "bothReady() must require both flags, not just local";
}


TEST(ReadySyncTracker, SetRemoteReady_OnlyAffectsRemote)
{
	ReadySyncTracker tracker;
	tracker.setRemoteReady();

	EXPECT_FALSE(tracker.isLocalReady());
	EXPECT_TRUE(tracker.isRemoteReady());
	EXPECT_FALSE(tracker.bothReady());
}


TEST(ReadySyncTracker, BothReady_TrueOnlyWhenBothFlagsSet)
{
	ReadySyncTracker tracker;
	tracker.setLocalReady();
	tracker.setRemoteReady();

	EXPECT_TRUE(tracker.bothReady());
}


TEST(ReadySyncTracker, SetReady_WithFalseArgument_ClearsFlag)
{
	ReadySyncTracker tracker;
	tracker.setLocalReady(true);
	ASSERT_TRUE(tracker.isLocalReady());

	tracker.setLocalReady(false);
	EXPECT_FALSE(tracker.isLocalReady());
}


TEST(ReadySyncTracker, Reset_ClearsBothFlags)
{
	ReadySyncTracker tracker;
	tracker.setLocalReady();
	tracker.setRemoteReady();
	ASSERT_TRUE(tracker.bothReady());

	tracker.reset();

	EXPECT_FALSE(tracker.isLocalReady());
	EXPECT_FALSE(tracker.isRemoteReady());
	EXPECT_FALSE(tracker.bothReady());
}
