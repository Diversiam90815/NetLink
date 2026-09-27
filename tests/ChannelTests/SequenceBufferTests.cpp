#include <gtest/gtest.h>

#include <string>

#include "Channel/Queue/SequenceBuffer.h"

using namespace netlink::channel;


namespace ChannelTests
{

TEST(SequenceBuffer, InsertFindErase)
{
	SequenceBuffer<std::string, 8> buffer;

	buffer.insert(5, "five");
	buffer.insert(6, "six");

	ASSERT_NE(buffer.find(5), nullptr);
	EXPECT_EQ(*buffer.find(5), "five");
	EXPECT_TRUE(buffer.contains(6));
	EXPECT_EQ(buffer.size(), 2u);

	EXPECT_TRUE(buffer.erase(5));
	EXPECT_FALSE(buffer.contains(5));
	EXPECT_FALSE(buffer.erase(5)) << "Erasing twice has no effect";
	EXPECT_EQ(buffer.size(), 1u);
}


TEST(SequenceBuffer, SlotOnlyAnswersForItsOwnSeq)
{
	SequenceBuffer<int, 8> buffer;
	buffer.insert(3, 30);

	// 11 maps to the same slot as 3
	EXPECT_FALSE(buffer.contains(11)) << "A different seq in the same slot must not be found";
	EXPECT_FALSE(buffer.erase(11));
	EXPECT_TRUE(buffer.contains(3));
}


TEST(SequenceBuffer, NewerSeqReplacesTheStaleSlot)
{
	SequenceBuffer<int, 8> buffer;
	buffer.insert(3, 30);
	buffer.insert(11, 110);

	EXPECT_FALSE(buffer.contains(3)) << "The older entry fell out of the window";
	ASSERT_TRUE(buffer.contains(11));
	EXPECT_EQ(*buffer.find(11), 110);
	EXPECT_EQ(buffer.size(), 1u);
}


TEST(SequenceBuffer, TakeRemovesAndReturns)
{
	SequenceBuffer<std::string, 4> buffer;
	buffer.insert(1, "one");

	auto taken = buffer.take(1);
	ASSERT_TRUE(taken.has_value());
	EXPECT_EQ(*taken, "one");
	EXPECT_TRUE(buffer.empty());
	EXPECT_FALSE(buffer.take(1).has_value());
}


TEST(SequenceBuffer, WorksWithLarge64BitSeqs)
{
	SequenceBuffer<int, 16> buffer;
	const uint64_t			seq = 0xFFFF'FFFF'FFFF'FF00ull;

	buffer.insert(seq, 1);
	buffer.insert(seq + 1, 2);

	EXPECT_TRUE(buffer.contains(seq));
	EXPECT_TRUE(buffer.contains(seq + 1));
	EXPECT_FALSE(buffer.contains(seq + 16));
}


TEST(SequenceBuffer, ForEachVisitsAndCanErase)
{
	SequenceBuffer<int, 8> buffer;
	for (uint64_t seq = 10; seq < 14; ++seq)
		buffer.insert(seq, static_cast<int>(seq));

	int visited = 0;
	buffer.forEach(
		[&](uint64_t seq, int &value)
		{
			++visited;
			EXPECT_EQ(static_cast<uint64_t>(value), seq);
			return seq % 2 == 0; // erase odd ones
		});

	EXPECT_EQ(visited, 4);
	EXPECT_EQ(buffer.size(), 2u);
	EXPECT_TRUE(buffer.contains(10));
	EXPECT_FALSE(buffer.contains(11));
}


TEST(SequenceBuffer, ClearEmptiesEverything)
{
	SequenceBuffer<int, 4> buffer;
	buffer.insert(1, 1);
	buffer.insert(2, 2);
	buffer.clear();

	EXPECT_TRUE(buffer.empty());
	EXPECT_FALSE(buffer.contains(1));
}

} // namespace ChannelTests
