#include <gtest/gtest.h>

#include <vector>

#include "Channel/Fragmentation/FragmentationService.h"

using namespace netlink;
using namespace netlink::channel;


namespace ChannelTests
{

static constexpr size_t		MaxBody = 100;

static std::vector<uint8_t> makeBody(size_t size)
{
	std::vector<uint8_t> body(size);
	for (size_t i = 0; i < size; ++i)
		body[i] = static_cast<uint8_t>(i * 31 + 7);
	return body;
}


// Every fragment of a body, in order
static std::vector<Fragment> split(const std::span<const uint8_t> body, const size_t maxFragmentBody)
{
	std::vector<Fragment> fragments;
	const size_t		  count = FragmentationService::fragmentCount(body.size(), maxFragmentBody);

	for (size_t i = 0; i < count; ++i)
		fragments.push_back(FragmentationService::fragmentAt(body, i, maxFragmentBody));

	return fragments;
}


TEST(FragmentationService, FragmentCount)
{
	EXPECT_EQ(FragmentationService::fragmentCount(0, MaxBody), 1u) << "An empty message still is one packet";
	EXPECT_EQ(FragmentationService::fragmentCount(1, MaxBody), 1u);
	EXPECT_EQ(FragmentationService::fragmentCount(100, MaxBody), 1u);
	EXPECT_EQ(FragmentationService::fragmentCount(101, MaxBody), 2u);
	EXPECT_EQ(FragmentationService::fragmentCount(MaxBody * FragmentationService::MaxFragments + 1, MaxBody), 0u) << "More fragments than the wire can number";
	EXPECT_EQ(FragmentationService::fragmentCount(10, 0), 0u);
}


TEST(FragmentationService, SmallMessageIsNotFragmented)
{
	const auto body		 = makeBody(50);
	const auto fragments = split(body, MaxBody);

	ASSERT_EQ(fragments.size(), 1u);
	EXPECT_FALSE(fragments[0].isFragmented());
	EXPECT_TRUE(fragments[0].isLast());
	EXPECT_EQ(std::vector<uint8_t>(fragments[0].body.begin(), fragments[0].body.end()), body);
}


TEST(FragmentationService, SplitCoversTheWholeBody)
{
	const auto body		 = makeBody(1050);
	const auto fragments = split(body, MaxBody);

	ASSERT_EQ(fragments.size(), 11u);

	std::vector<uint8_t> joined;
	for (const auto &fragment : fragments)
	{
		EXPECT_EQ(fragment.count, 11);
		EXPECT_LE(fragment.body.size(), MaxBody);
		joined.insert(joined.end(), fragment.body.begin(), fragment.body.end());
	}

	EXPECT_EQ(joined, body);
	EXPECT_TRUE(fragments.back().isLast());
	EXPECT_EQ(fragments.back().body.size(), 50u);
}


TEST(FragmentationService, FragmentAt_PointsIntoTheBody)
{
	const auto body = makeBody(250);

	for (size_t index = 0; index < 3; ++index)
	{
		const Fragment fragment = FragmentationService::fragmentAt(body, index, MaxBody);

		EXPECT_EQ(fragment.index, index);
		EXPECT_EQ(fragment.count, 3);
		EXPECT_EQ(fragment.body.data(), body.data() + index * MaxBody) << "Fragments are views: splitting must not copy the message";
		EXPECT_EQ(fragment.body.size(), index < 2 ? MaxBody : 50u);
	}
}

} // namespace ChannelTests
