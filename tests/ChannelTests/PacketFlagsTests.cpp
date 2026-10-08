#include <gtest/gtest.h>

#include "Channel/Protocol/PacketFlags.h"

using namespace netlink::channel;


namespace ChannelTests
{

TEST(PacketFlags, DefaultIsAControlDataPacket)
{
	const PacketFlags flags;

	EXPECT_EQ(flags.raw(), 0);
	EXPECT_EQ(flags.kind(), PacketKind::Data);
	EXPECT_EQ(flags.lane(), Lane::Control);
	EXPECT_FALSE(flags.isFragmented());
	EXPECT_FALSE(flags.isLastFragment());
	EXPECT_FALSE(flags.isPaused());
	EXPECT_TRUE(flags.isValid());
}


TEST(PacketFlags, KindAndLaneShareTheLowFourBits)
{
	for (const auto kind : {PacketKind::Data, PacketKind::Ack, PacketKind::Ping})
	{
		for (const auto lane : {Lane::Control, Lane::Reliable, Lane::Bulk, Lane::Media})
		{
			const auto flags = PacketFlags{}.setKind(kind).setLane(lane);

			EXPECT_EQ(flags.kind(), kind);
			EXPECT_EQ(flags.lane(), lane);
			EXPECT_EQ(flags.raw() & 0xF0, 0) << "Neither touches the flag bits";
		}
	}

	EXPECT_EQ(PacketFlags{}.setKind(PacketKind::Ack).setLane(Lane::Bulk).raw(), 0x09) << "kind in bits 0-1, lane in bits 2-3";
}


TEST(PacketFlags, BitsAreSetAndClearedIndependently)
{
	PacketFlags flags = PacketFlags::data(Lane::Reliable);

	flags.setFragment(true, false);
	EXPECT_TRUE(flags.isFragmented());
	EXPECT_FALSE(flags.isLastFragment());

	flags.setFragment(true, true);
	EXPECT_TRUE(flags.isLastFragment());
	EXPECT_EQ(flags.lane(), Lane::Reliable) << "Setting one field must not disturb another";
	EXPECT_EQ(flags.kind(), PacketKind::Data);

	flags.setFragment(false, true);
	EXPECT_FALSE(flags.isFragmented());
	EXPECT_FALSE(flags.isLastFragment()) << "Only a fragment can be the last one";
}


TEST(PacketFlags, FactoriesProduceValidFlags)
{
	for (const auto lane : {Lane::Control, Lane::Reliable, Lane::Bulk, Lane::Media})
		EXPECT_TRUE(PacketFlags::data(lane).isValid());

	for (const auto lane : {Lane::Control, Lane::Reliable, Lane::Bulk})
	{
		EXPECT_TRUE(PacketFlags::ack(lane).isValid());
		EXPECT_TRUE(PacketFlags::ack(lane, true).isValid());
		EXPECT_TRUE(PacketFlags::ack(lane, true).isPaused());
		EXPECT_FALSE(PacketFlags::ack(lane).isPaused());
		EXPECT_TRUE(PacketFlags::ping(lane).isValid());
	}

	EXPECT_TRUE(PacketFlags::data(Lane::Media).setFragment(true, false).isValid()) << "Media messages are fragmented as well";
}


TEST(PacketFlags, ImpossibleCombinationsAreRejected)
{
	EXPECT_FALSE(PacketFlags::fromRaw(0x80).isValid()) << "Encrypted datagrams are not understood by this version";

	EXPECT_FALSE(PacketFlags::data(Lane::Reliable).set(FlagBit::LastFragment).isValid()) << "Last fragment of nothing";
	EXPECT_FALSE(PacketFlags::ack(Lane::Reliable).set(FlagBit::Fragmented).isValid()) << "Only Data is fragmented";
	EXPECT_FALSE(PacketFlags::ping(Lane::Reliable).set(FlagBit::Fragmented).isValid());

	EXPECT_FALSE(PacketFlags::data(Lane::Reliable).set(FlagBit::Paused).isValid()) << "Only an Ack asks for a pause";
	EXPECT_FALSE(PacketFlags::ping(Lane::Reliable).set(FlagBit::Paused).isValid());

	EXPECT_FALSE(PacketFlags::ack(Lane::Media).isValid()) << "Media is never acknowledged";
	EXPECT_FALSE(PacketFlags::ping(Lane::Media).isValid());

	EXPECT_TRUE(PacketFlags{}.setKind(PacketKind::Beacon).isValid());
	EXPECT_FALSE(PacketFlags{}.setKind(PacketKind::Beacon).setLane(Lane::Reliable).isValid()) << "A beacon belongs to no lane";
}


TEST(PacketFlags, RawRoundTrip)
{
	const auto flags = PacketFlags::data(Lane::Bulk).setFragment(true, true);
	EXPECT_EQ(PacketFlags::fromRaw(flags.raw()), flags);
}

} // namespace ChannelTests
