#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <random>
#include <vector>

#include "Channel/Fragmentation/MediaAssembler.h"

using namespace netlink;
using namespace netlink::channel;


namespace ChannelTests
{

class MediaAssemblerTest : public ::testing::Test
{
protected:
	struct Packet
	{
		PacketHeader			 header;
		std::span<const uint8_t> body;
	};

	static std::vector<uint8_t> makeBody(size_t size, uint8_t seed = 7)
	{
		std::vector<uint8_t> body(size);
		for (size_t i = 0; i < size; ++i)
			body[i] = static_cast<uint8_t>(i * 31 + seed);
		return body;
	}

	// The datagrams of one message, the first of which has the given seq
	static std::vector<Packet> packetsOf(const std::vector<uint8_t> &body, uint64_t firstSeq, uint32_t tag = 0)
	{
		const size_t		count = fragmentsOf(body.size());
		std::vector<Packet> packets;

		for (size_t index = 0; index < count; ++index)
		{
			const size_t offset = index * MaxFragmentBody;

			PacketHeader header;
			header.flags	   = PacketFlags::data(Lane::Media);
			header.srcStreamID = 1;
			header.seq		   = firstSeq + index;
			header.tag		   = tag;

			if (count > 1)
			{
				header.flags.setFragment(true, index + 1 == count);
				header.fragIndex   = static_cast<uint16_t>(index);
				header.fragCount   = static_cast<uint16_t>(count);
				header.totalLength = static_cast<uint32_t>(body.size());
			}

			packets.push_back({header, std::span(body).subspan(offset, std::min(MaxFragmentBody, body.size() - offset))});
		}

		return packets;
	}

	std::optional<AssembledMessage> feed(const std::vector<Packet> &packets)
	{
		std::optional<AssembledMessage> result;

		for (const auto &[header, body] : packets)
		{
			if (auto message = assembler.accept(header, body))
				result = std::move(message);
		}

		return result;
	}

	MediaAssembler assembler;
};


TEST_F(MediaAssemblerTest, UnfragmentedMessage_IsDeliveredAtOnce)
{
	const auto body	   = makeBody(200);
	const auto message = feed(packetsOf(body, 1, 9));

	ASSERT_TRUE(message.has_value());
	EXPECT_EQ(message->body, body);
	EXPECT_EQ(message->tag, 9u);
	EXPECT_EQ(assembler.inProgress(), 0u);
}


TEST_F(MediaAssemblerTest, Fragments_AreJoinedInAnyOrder)
{
	const auto	 body	 = makeBody(8000);
	auto		 packets = packetsOf(body, 100, 3);
	std::mt19937 random(5);
	std::ranges::shuffle(packets, random);

	for (size_t i = 0; i + 1 < packets.size(); ++i)
		EXPECT_FALSE(assembler.accept(packets[i].header, packets[i].body).has_value());

	const auto message = assembler.accept(packets.back().header, packets.back().body);

	ASSERT_TRUE(message.has_value()) << "Complete with the last fragment that was missing, whichever that is";
	EXPECT_EQ(message->body, body);
	EXPECT_EQ(message->tag, 3u);
	EXPECT_EQ(assembler.inProgress(), 0u);
}


TEST_F(MediaAssemblerTest, LargestMessage_IsPutTogether)
{
	const auto body	   = makeBody(internal::MaxMediaPayload);
	const auto message = feed(packetsOf(body, 1));

	ASSERT_TRUE(message.has_value());
	EXPECT_EQ(message->body, body);
}


TEST_F(MediaAssemblerTest, MessagesInterleave)
{
	const auto first	= makeBody(3000, 1);
	const auto second	= makeBody(3000, 2);
	const auto packetsA = packetsOf(first, 10);
	const auto packetsB = packetsOf(second, 20);

	EXPECT_FALSE(assembler.accept(packetsA[0].header, packetsA[0].body).has_value());
	EXPECT_FALSE(assembler.accept(packetsB[0].header, packetsB[0].body).has_value());
	EXPECT_FALSE(assembler.accept(packetsB[1].header, packetsB[1].body).has_value());
	EXPECT_FALSE(assembler.accept(packetsA[1].header, packetsA[1].body).has_value());
	EXPECT_EQ(assembler.inProgress(), 2u);

	const auto b = assembler.accept(packetsB[2].header, packetsB[2].body);
	const auto a = assembler.accept(packetsA[2].header, packetsA[2].body);

	ASSERT_TRUE(a && b);
	EXPECT_EQ(a->body, first);
	EXPECT_EQ(b->body, second);
}


TEST_F(MediaAssemblerTest, MissingFragment_NeverDelivers)
{
	const auto body	   = makeBody(5000);
	auto	   packets = packetsOf(body, 1);
	packets.erase(packets.begin() + 2);

	EXPECT_FALSE(feed(packets).has_value());
	EXPECT_EQ(assembler.inProgress(), 1u) << "It waits for a fragment that may never come";
}


TEST_F(MediaAssemblerTest, NewMessage_EvictsTheOldestIncompleteOne)
{
	// More messages lose a fragment than can be kept at the same time
	std::vector<std::vector<uint8_t>> bodies;
	for (uint8_t i = 0; i < MediaAssemblySlots + 2; ++i)
		bodies.push_back(makeBody(3000, i));

	for (size_t i = 0; i < bodies.size(); ++i)
	{
		const auto packets = packetsOf(bodies[i], 100 * (i + 1));
		EXPECT_FALSE(assembler.accept(packets[0].header, packets[0].body).has_value());
	}

	EXPECT_EQ(assembler.inProgress(), MediaAssemblySlots);
	EXPECT_EQ(assembler.abandoned(), 2u);

	// The oldest one was given up: its remaining fragments start over and cannot complete it
	const auto oldest = packetsOf(bodies[0], 100);
	EXPECT_FALSE(assembler.accept(oldest[1].header, oldest[1].body).has_value());
	EXPECT_FALSE(assembler.accept(oldest[2].header, oldest[2].body).has_value()) << "A straggler of a message older than everything in progress is ignored";

	// The newest one is still there
	const auto newest = packetsOf(bodies.back(), 100 * bodies.size());
	EXPECT_FALSE(assembler.accept(newest[1].header, newest[1].body).has_value());
	const auto message = assembler.accept(newest[2].header, newest[2].body);

	ASSERT_TRUE(message.has_value());
	EXPECT_EQ(message->body, bodies.back());
}


TEST_F(MediaAssemblerTest, DuplicatedFragment_IsIgnored)
{
	const auto body	   = makeBody(3000);
	const auto packets = packetsOf(body, 1);

	EXPECT_FALSE(assembler.accept(packets[0].header, packets[0].body).has_value());
	EXPECT_FALSE(assembler.accept(packets[0].header, packets[0].body).has_value());
	EXPECT_FALSE(assembler.accept(packets[1].header, packets[1].body).has_value());

	const auto message = assembler.accept(packets[2].header, packets[2].body);
	ASSERT_TRUE(message.has_value());
	EXPECT_EQ(message->body, body);
}


TEST_F(MediaAssemblerTest, InconsistentFragments_DoNotDeliver)
{
	const auto body				  = makeBody(3000);
	auto	   packets			  = packetsOf(body, 1);

	// The length the first fragment announces is not what the fragments add up to
	packets[0].header.totalLength = 2 * MaxFragmentBody + 5;
	EXPECT_FALSE(feed(packets).has_value());
	EXPECT_EQ(assembler.inProgress(), 0u) << "Dropped, not kept forever";

	// A fragment that claims another count than the message it belongs to
	packets = packetsOf(body, 50);
	EXPECT_FALSE(assembler.accept(packets[0].header, packets[0].body).has_value());
	packets[1].header.fragCount = 5;
	EXPECT_FALSE(assembler.accept(packets[1].header, packets[1].body).has_value());

	// A middle fragment that is not full
	packets = packetsOf(body, 80);
	EXPECT_FALSE(assembler.accept(packets[1].header, packets[1].body.first(10)).has_value());
}

} // namespace ChannelTests
