#include <gtest/gtest.h>

#include <vector>

#include "Channel/Protocol/AckRanges.h"

using namespace netlink::channel;


namespace ChannelTests
{

TEST(AckRanges, ConsecutiveSeqs_BecomeOneRange)
{
	std::vector<uint64_t> seqs{5, 6, 7, 8};

	const auto			  ranges = toRanges(seqs);

	ASSERT_EQ(ranges.size(), 1u);
	EXPECT_EQ(ranges[0], (SeqRange{5, 4}));
	EXPECT_EQ(ranges[0].last(), 8u);
}


TEST(AckRanges, Gaps_StartANewRange)
{
	std::vector<uint64_t> seqs{1, 2, 4, 7, 8};

	const auto			  ranges = toRanges(seqs);

	ASSERT_EQ(ranges.size(), 3u);
	EXPECT_EQ(ranges[0], (SeqRange{1, 2}));
	EXPECT_EQ(ranges[1], (SeqRange{4, 1}));
	EXPECT_EQ(ranges[2], (SeqRange{7, 2}));
}


TEST(AckRanges, UnorderedAndDuplicateSeqs_AreSortedAndMerged)
{
	std::vector<uint64_t> seqs{9, 3, 4, 3, 10, 9, 5};

	const auto			  ranges = toRanges(seqs);

	ASSERT_EQ(ranges.size(), 2u);
	EXPECT_EQ(ranges[0], (SeqRange{3, 3}));
	EXPECT_EQ(ranges[1], (SeqRange{9, 2}));
}


TEST(AckRanges, NoSeqs_NoRanges)
{
	std::vector<uint64_t> seqs;
	EXPECT_TRUE(toRanges(seqs).empty());
}


TEST(AckRanges, ARange_HoldsAtMost65535Seqs)
{
	std::vector<uint64_t> seqs;
	for (uint64_t seq = 1; seq <= 70'000; ++seq)
		seqs.push_back(seq);

	const auto ranges = toRanges(seqs);

	ASSERT_EQ(ranges.size(), 2u);
	EXPECT_EQ(ranges[0], (SeqRange{1, UINT16_MAX}));
	EXPECT_EQ(ranges[1].first, uint64_t{UINT16_MAX} + 1);
	EXPECT_EQ(ranges[1].last(), 70'000u);
}


TEST(AckRanges, RoundTripOverTheWire)
{
	const AckBody ack{.serial = 0x01020304, .mediaReceived = 77, .ranges = {{3, 3}, {0x0102030405060708ull, 500}, {99, 1}}};

	const auto	  body = encodeAck(ack, 10);

	ASSERT_EQ(body.size(), AckFieldsSize + ack.ranges.size() * SeqRangeSize);
	EXPECT_EQ(body[0], 0x01) << "Big endian, like the packet header";
	EXPECT_EQ(body[3], 0x04);
	EXPECT_EQ(body[AckFieldsSize + SeqRangeSize], 0x01);
	EXPECT_EQ(body[AckFieldsSize + SeqRangeSize + 7], 0x08);

	const auto decoded = decodeAck(body);
	ASSERT_TRUE(decoded.has_value());
	EXPECT_EQ(decoded->serial, ack.serial);
	EXPECT_EQ(decoded->mediaReceived, 77u);
	EXPECT_EQ(decoded->ranges, ack.ranges);
}


TEST(AckRanges, AckWithoutRanges_IsOnlyItsTwoFields)
{
	const auto body = encodeAck({.serial = 5, .mediaReceived = 0, .ranges = {}}, 10);
	EXPECT_EQ(body.size(), AckFieldsSize);

	const auto decoded = decodeAck(body);
	ASSERT_TRUE(decoded.has_value());
	EXPECT_EQ(decoded->serial, 5u);
	EXPECT_TRUE(decoded->ranges.empty());
}


TEST(AckRanges, MoreRangesThanFit_KeepsTheLowestSeqs)
{
	AckBody ack;
	for (uint64_t i = 0; i < 20; ++i)
		ack.ranges.push_back({10 + i * 2, 1});

	const auto decoded = decodeAck(encodeAck(ack, 5));

	ASSERT_TRUE(decoded.has_value());
	ASSERT_EQ(decoded->ranges.size(), 5u);
	EXPECT_EQ(decoded->ranges.front().first, 10u);
	EXPECT_EQ(decoded->ranges.back().first, 18u) << "The sender needs the oldest gaps first";
}


TEST(AckRanges, RejectsBrokenBodies)
{
	const auto body = encodeAck({.serial = 1, .mediaReceived = 0, .ranges = {{5, 2}}}, 10);
	ASSERT_TRUE(decodeAck(body).has_value());

	auto truncated = body;
	truncated.pop_back();
	EXPECT_FALSE(decodeAck(truncated).has_value()) << "Not a whole number of ranges";

	EXPECT_FALSE(decodeAck(std::span(body.data(), AckFieldsSize - 1)).has_value()) << "Without its two fields";

	EXPECT_FALSE(decodeAck(encodeAck({.serial = 1, .mediaReceived = 0, .ranges = {{5, 0}}}, 10)).has_value()) << "Empty ranges are never sent";
	EXPECT_FALSE(decodeAck(encodeAck({.serial = 1, .mediaReceived = 0, .ranges = {{0, 3}}}, 10)).has_value()) << "Seqs start at 1";
	EXPECT_FALSE(decodeAck(encodeAck({.serial = 1, .mediaReceived = 0, .ranges = {{UINT64_MAX - 1, 5}}}, 10)).has_value()) << "A range must not wrap around";
}

} // namespace ChannelTests
