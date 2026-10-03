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
	const std::vector<SeqRange> ranges{{1, 3}, {0x0102030405060708ull, 500}, {99, 1}};

	std::vector<uint8_t>		body;
	for (const auto &range : ranges)
		appendRange(body, range);

	ASSERT_EQ(body.size(), ranges.size() * SeqRangeSize);
	EXPECT_EQ(body[SeqRangeSize], 0x01) << "Big endian, like the packet header";
	EXPECT_EQ(body[SeqRangeSize + 7], 0x08);

	const auto decoded = decodeRanges(body);
	ASSERT_TRUE(decoded.has_value());
	EXPECT_EQ(*decoded, ranges);
}


TEST(AckRanges, EmptyBody_IsNoRanges)
{
	const auto decoded = decodeRanges({});

	ASSERT_TRUE(decoded.has_value());
	EXPECT_TRUE(decoded->empty());
}


TEST(AckRanges, RejectsBrokenBodies)
{
	std::vector<uint8_t> body;
	appendRange(body, {5, 2});

	auto truncated = body;
	truncated.pop_back();
	EXPECT_FALSE(decodeRanges(truncated).has_value()) << "Not a whole number of ranges";

	std::vector<uint8_t> emptyRange;
	appendRange(emptyRange, {5, 0});
	EXPECT_FALSE(decodeRanges(emptyRange).has_value()) << "Empty ranges are never sent";

	std::vector<uint8_t> seqZero;
	appendRange(seqZero, {0, 3});
	EXPECT_FALSE(decodeRanges(seqZero).has_value()) << "Seqs start at 1";

	std::vector<uint8_t> wrapping;
	appendRange(wrapping, {UINT64_MAX - 1, 5});
	EXPECT_FALSE(decodeRanges(wrapping).has_value()) << "A range must not wrap around";
}

} // namespace ChannelTests
