#include <gtest/gtest.h>

#include <algorithm>
#include <numeric>
#include <random>
#include <vector>

#include "TestIp.h"
#include "Channel/Fragmentation/FragmentationService.h"

using namespace netlink;
using namespace netlink::channel;


namespace ChannelTests
{

static std::vector<uint8_t> makeBody(size_t size)
{
	std::vector<uint8_t> body(size);
	for (size_t i = 0; i < size; ++i)
		body[i] = static_cast<uint8_t>(i * 31 + 7);
	return body;
}


// Header the link would put on the given fragment, taking consecutive seqs starting at firstSeq
static PacketHeader headerFor(const Fragment &fragment, uint64_t firstSeq, ChannelId channel = ChannelId::Application)
{
	PacketHeader header;
	header.flags	   = PacketFlags::data(channel, true);
	header.srcStreamID = 1;
	header.seq		   = firstSeq + fragment.index;

	if (fragment.isFragmented())
	{
		header.flags.setFragment(true, fragment.isLast());
		header.fragIndex = fragment.index;
		header.fragCount = fragment.count;
	}

	return header;
}


class FragmentationServiceTest : public ::testing::Test
{
protected:
	static constexpr size_t			  MaxBody = 100;

	FragmentationService			  service;
	net::SocketAddress				  peer{ipv4("10.0.0.2"), 50000};

	std::optional<ReassembledMessage> feed(const std::vector<Fragment> &fragments, const std::vector<size_t> &order, uint64_t firstSeq = 1)
	{
		std::optional<ReassembledMessage> result;

		for (size_t index : order)
		{
			auto message = service.accept(peer, headerFor(fragments[index], firstSeq), fragments[index].body);
			if (message)
			{
				EXPECT_FALSE(result.has_value()) << "A message must complete exactly once";
				result = std::move(message);
			}
		}

		return result;
	}
};


TEST_F(FragmentationServiceTest, FragmentCount)
{
	EXPECT_EQ(FragmentationService::fragmentCount(0, MaxBody), 1u) << "An empty message still is one packet";
	EXPECT_EQ(FragmentationService::fragmentCount(1, MaxBody), 1u);
	EXPECT_EQ(FragmentationService::fragmentCount(100, MaxBody), 1u);
	EXPECT_EQ(FragmentationService::fragmentCount(101, MaxBody), 2u);
	EXPECT_EQ(FragmentationService::fragmentCount(MaxBody * FragmentationService::MaxFragments + 1, MaxBody), 0u) << "More fragments than the wire can number";
	EXPECT_EQ(FragmentationService::fragmentCount(10, 0), 0u);
}


TEST_F(FragmentationServiceTest, SmallMessageIsNotFragmented)
{
	const auto body		 = makeBody(50);
	const auto fragments = FragmentationService::split(body, MaxBody);

	ASSERT_EQ(fragments.size(), 1u);
	EXPECT_FALSE(fragments[0].isFragmented());

	auto message = feed(fragments, {0});
	ASSERT_TRUE(message.has_value()) << "An unfragmented packet is a complete message";
	EXPECT_EQ(message->body, body);
	EXPECT_EQ(message->channel, ChannelId::Application);
}


TEST_F(FragmentationServiceTest, SplitCoversTheWholeBody)
{
	const auto body		 = makeBody(1050);
	const auto fragments = FragmentationService::split(body, MaxBody);

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


TEST_F(FragmentationServiceTest, ReassemblesInOrder)
{
	const auto			body	  = makeBody(1050);
	const auto			fragments = FragmentationService::split(body, MaxBody);

	std::vector<size_t> order(fragments.size());
	std::iota(order.begin(), order.end(), 0);

	auto message = feed(fragments, order);
	ASSERT_TRUE(message.has_value());
	EXPECT_EQ(message->body, body);
	EXPECT_EQ(service.partialMessageCount(peer), 0u) << "Completed messages must not leave state behind";
}


TEST_F(FragmentationServiceTest, ReassemblesOutOfOrder)
{
	const auto			body	  = makeBody(2000);
	const auto			fragments = FragmentationService::split(body, MaxBody);

	std::vector<size_t> order(fragments.size());
	std::iota(order.begin(), order.end(), 0);
	std::shuffle(order.begin(), order.end(), std::mt19937(7));

	auto message = feed(fragments, order, 500);
	ASSERT_TRUE(message.has_value());
	EXPECT_EQ(message->body, body);
}


TEST_F(FragmentationServiceTest, DuplicateFragmentsAreIgnored)
{
	const auto body		 = makeBody(250);
	const auto fragments = FragmentationService::split(body, MaxBody);

	auto	   message	 = feed(fragments, {0, 0, 1, 1, 2});
	ASSERT_TRUE(message.has_value());
	EXPECT_EQ(message->body, body);
}


TEST_F(FragmentationServiceTest, InterleavedMessagesStaySeparate)
{
	const auto first  = makeBody(250);
	auto	   second = makeBody(250);
	std::reverse(second.begin(), second.end());

	const auto						  fragmentsA = FragmentationService::split(first, MaxBody);
	const auto						  fragmentsB = FragmentationService::split(second, MaxBody);

	std::optional<ReassembledMessage> a, b;
	for (size_t i = 0; i < 3; ++i)
	{
		if (auto m = service.accept(peer, headerFor(fragmentsA[i], 10), fragmentsA[i].body))
			a = std::move(m);
		if (auto m = service.accept(peer, headerFor(fragmentsB[i], 20), fragmentsB[i].body))
			b = std::move(m);
	}

	ASSERT_TRUE(a && b);
	EXPECT_EQ(a->body, first);
	EXPECT_EQ(b->body, second);
}


TEST_F(FragmentationServiceTest, InconsistentFragmentCountAbortsTheMessage)
{
	const auto fragments = FragmentationService::split(makeBody(250), MaxBody);

	EXPECT_FALSE(service.accept(peer, headerFor(fragments[0], 1), fragments[0].body).has_value());

	PacketHeader bogus = headerFor(fragments[1], 1);
	bogus.fragCount	   = 5;
	EXPECT_FALSE(service.accept(peer, bogus, fragments[1].body).has_value());
	EXPECT_EQ(service.partialMessageCount(peer), 0u) << "A contradicting fragment abandons the message";
}


TEST_F(FragmentationServiceTest, OversizedMessageIsDropped)
{
	FragmentationService small(150);
	const auto			 fragments = FragmentationService::split(makeBody(250), MaxBody);

	for (const auto &fragment : fragments)
		EXPECT_FALSE(small.accept(peer, headerFor(fragment, 1), fragment.body).has_value());

	EXPECT_EQ(small.partialMessageCount(peer), 1u) << "Only the fragment arriving after the abort starts a new partial";
}


TEST_F(FragmentationServiceTest, PartialMessagesPerPeerAreBounded)
{
	const auto fragments = FragmentationService::split(makeBody(250), MaxBody);

	for (uint64_t message = 0; message < FragmentationService::MaxPartialMessagesPerPeer + 10; ++message)
		service.accept(peer, headerFor(fragments[0], 1 + message * 3), fragments[0].body);

	EXPECT_EQ(service.partialMessageCount(peer), FragmentationService::MaxPartialMessagesPerPeer);
}


TEST_F(FragmentationServiceTest, ResetForgetsThePeer)
{
	const auto fragments = FragmentationService::split(makeBody(250), MaxBody);
	service.accept(peer, headerFor(fragments[0], 1), fragments[0].body);

	service.reset(peer);

	EXPECT_EQ(service.partialMessageCount(peer), 0u);
	EXPECT_FALSE(service.accept(peer, headerFor(fragments[1], 1), fragments[1].body).has_value());
	EXPECT_FALSE(service.accept(peer, headerFor(fragments[2], 1), fragments[2].body).has_value()) << "Fragment 0 was forgotten";
}


TEST_F(FragmentationServiceTest, LargeMessageRoundTrip)
{
	const size_t					  maxBody	= 1176;
	const auto						  body		= makeBody(size_t{4} * 1024 * 1024);
	const auto						  fragments = FragmentationService::split(body, maxBody);

	std::optional<ReassembledMessage> message;
	for (const auto &fragment : fragments)
	{
		if (auto m = service.accept(peer, headerFor(fragment, 1), fragment.body))
			message = std::move(m);
	}

	ASSERT_TRUE(message.has_value());
	EXPECT_EQ(message->body, body);
}

} // namespace ChannelTests
