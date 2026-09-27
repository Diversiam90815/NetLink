#include <gtest/gtest.h>

#include "Channel/Protocol/PacketFlags.h"

using namespace netlink::channel;


namespace ChannelTests
{

TEST(PacketFlags, DefaultIsAnUnreliableControlDataPacket)
{
	PacketFlags flags;

	EXPECT_EQ(flags.raw(), 0);
	EXPECT_EQ(flags.kind(), PacketKind::Data);
	EXPECT_FALSE(flags.isReliable());
	EXPECT_FALSE(flags.isFragmented());
	EXPECT_FALSE(flags.isLastFragment());
	EXPECT_EQ(flags.channel(), ChannelId::Control);
	EXPECT_TRUE(flags.isValid());
}


TEST(PacketFlags, EveryKindFitsIntoTheLowThreeBits)
{
	for (auto kind : {PacketKind::Data, PacketKind::DataAck, PacketKind::AckAck, PacketKind::Heartbeat})
	{
		PacketFlags flags;
		flags.setKind(kind);

		EXPECT_EQ(flags.kind(), kind);
		EXPECT_EQ(flags.raw() & ~PacketFlags::KindMask, 0) << "The kind must not touch any flag bit";
	}
}


TEST(PacketFlags, BitsAreSetAndClearedIndependently)
{
	PacketFlags flags = PacketFlags::data(ChannelId::Application, true);
	flags.setFragment(true, true);

	EXPECT_EQ(flags.raw(), (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6)) << "Wire layout: reliable=bit3, fragmented=bit4, last=bit5, application=bit6";
	EXPECT_TRUE(flags.isReliable());
	EXPECT_TRUE(flags.isFragmented());
	EXPECT_TRUE(flags.isLastFragment());
	EXPECT_EQ(flags.channel(), ChannelId::Application);

	flags.setReliable(false);
	EXPECT_FALSE(flags.isReliable());
	EXPECT_TRUE(flags.isFragmented()) << "Clearing one bit must leave the others alone";

	flags.setChannel(ChannelId::Control);
	EXPECT_EQ(flags.channel(), ChannelId::Control);

	flags.setKind(PacketKind::AckAck);
	EXPECT_EQ(flags.kind(), PacketKind::AckAck);
	EXPECT_TRUE(flags.isFragmented()) << "Changing the kind must leave the flag bits alone";
}


TEST(PacketFlags, LastFragmentRequiresFragmented)
{
	PacketFlags flags;
	flags.setFragment(false, true);

	EXPECT_FALSE(flags.isLastFragment()) << "An unfragmented packet is never flagged as a last fragment";
}


TEST(PacketFlags, FactoriesProduceValidFlags)
{
	EXPECT_TRUE(PacketFlags::data(ChannelId::Control, true).isValid());
	EXPECT_TRUE(PacketFlags::data(ChannelId::Application, false).isValid());
	EXPECT_TRUE(PacketFlags::ack(PacketKind::DataAck).isValid());
	EXPECT_TRUE(PacketFlags::ack(PacketKind::AckAck).isValid());
	EXPECT_TRUE(PacketFlags::heartbeat().isValid());
}


TEST(PacketFlags, ReservedKindsAndBitsAreRejected)
{
	for (uint8_t kind = 4; kind <= 7; ++kind)
		EXPECT_FALSE(PacketFlags::fromRaw(kind).isValid()) << "Kind " << int(kind) << " is reserved";

	EXPECT_FALSE(PacketFlags::fromRaw(0x80).isValid()) << "Bit 7 is reserved";
}


TEST(PacketFlags, ImpossibleCombinationsAreRejected)
{
	PacketFlags lastWithoutFragmented = PacketFlags::fromRaw(static_cast<uint8_t>(FlagBit::LastFragment) | static_cast<uint8_t>(FlagBit::Reliable));
	EXPECT_FALSE(lastWithoutFragmented.isValid());

	PacketFlags unreliableFragment = PacketFlags::data(ChannelId::Application, false);
	unreliableFragment.set(FlagBit::Fragmented);
	EXPECT_FALSE(unreliableFragment.isValid()) << "Only reliable data is fragmented";

	PacketFlags fragmentedAck = PacketFlags::ack(PacketKind::DataAck);
	fragmentedAck.set(FlagBit::Fragmented);
	EXPECT_FALSE(fragmentedAck.isValid()) << "Acknowledgements carry no content to fragment";

	PacketFlags unreliableAck;
	unreliableAck.setKind(PacketKind::DataAck);
	EXPECT_FALSE(unreliableAck.isValid()) << "Only reliable packets are acknowledged";

	PacketFlags reliableHeartbeat = PacketFlags::heartbeat();
	reliableHeartbeat.setReliable();
	EXPECT_FALSE(reliableHeartbeat.isValid()) << "Heartbeats are never acknowledged";
}


TEST(PacketFlags, RawRoundTrip)
{
	PacketFlags flags = PacketFlags::data(ChannelId::Application, true).setFragment(true, false);

	EXPECT_EQ(PacketFlags::fromRaw(flags.raw()), flags);
}

} // namespace ChannelTests
