#include <gtest/gtest.h>

#include <optional>
#include <vector>

#include "Channel/Fragmentation/MessageAssembler.h"

using namespace netlink;
using namespace netlink::channel;


namespace ChannelTests
{

class MessageAssemblerTest : public ::testing::Test
{
protected:
	// One packet of a stream, as a link would send it
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

	// The message cut the way a link cuts it
	static std::vector<Packet> packetsOf(const std::vector<uint8_t> &body, uint32_t tag = 0)
	{
		const size_t		count = fragmentsOf(body.size());
		std::vector<Packet> packets;

		for (size_t index = 0; index < count; ++index)
		{
			const size_t offset = index * MaxFragmentBody;

			PacketHeader header;
			header.flags	   = PacketFlags::data(Lane::Reliable);
			header.srcStreamID = 1;
			header.seq		   = 1 + index;
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

	// Feeds the message the way a stream delivers it: every fragment, in order
	static std::optional<AssembledMessage> feed(MessageAssembler &target, const std::vector<uint8_t> &body, uint32_t tag = 0)
	{
		std::optional<AssembledMessage> result;

		for (const auto &[header, fragment] : packetsOf(body, tag))
		{
			if (auto message = target.accept(header, fragment))
			{
				EXPECT_FALSE(result.has_value()) << "A message must complete exactly once";
				result = std::move(message);
			}
		}

		return result;
	}

	MessageAssembler assembler;
};


TEST_F(MessageAssemblerTest, UnfragmentedPacket_IsACompleteMessage)
{
	const auto body	   = makeBody(50);
	const auto message = feed(assembler, body, 42);

	ASSERT_TRUE(message.has_value());
	EXPECT_EQ(message->body, body);
	EXPECT_EQ(message->tag, 42u);
	EXPECT_FALSE(assembler.isAssembling());
}


TEST_F(MessageAssemblerTest, EmptyMessage_IsDelivered)
{
	const auto message = feed(assembler, {}, 5);

	ASSERT_TRUE(message.has_value());
	EXPECT_TRUE(message->body.empty());
	EXPECT_EQ(message->tag, 5u);
}


TEST_F(MessageAssemblerTest, Fragments_AreJoinedInOrder)
{
	const auto body	   = makeBody(10 * MaxFragmentBody + 500);
	const auto packets = packetsOf(body, 9);

	for (size_t i = 0; i + 1 < packets.size(); ++i)
	{
		EXPECT_FALSE(assembler.accept(packets[i].header, packets[i].body).has_value()) << "Not complete before the last fragment";
		EXPECT_TRUE(assembler.isAssembling());
	}

	const auto message = assembler.accept(packets.back().header, packets.back().body);

	ASSERT_TRUE(message.has_value());
	EXPECT_EQ(message->body, body);
	EXPECT_EQ(message->tag, 9u) << "The tag comes with the first fragment";
	EXPECT_FALSE(assembler.isAssembling()) << "Completed messages must not leave state behind";
}


TEST_F(MessageAssemblerTest, ConsecutiveMessages_StaySeparate)
{
	const auto first  = makeBody(3000, 1);
	const auto second = makeBody(30, 2);
	const auto third  = makeBody(5000, 3);

	const auto a	  = feed(assembler, first, 1);
	const auto b	  = feed(assembler, second, 2);
	const auto c	  = feed(assembler, third, 3);

	ASSERT_TRUE(a && b && c);
	EXPECT_EQ(a->body, first);
	EXPECT_EQ(b->body, second);
	EXPECT_EQ(c->body, third);
	EXPECT_EQ(c->tag, 3u);
}


TEST_F(MessageAssemblerTest, AbandonedMessage_IsReplacedByTheNextOne)
{
	// The sender started a new stream in the middle of a message and went on with the next one
	const auto abandonedBody = makeBody(4000, 1);
	const auto abandoned	 = packetsOf(abandonedBody);
	EXPECT_FALSE(assembler.accept(abandoned[0].header, abandoned[0].body).has_value());
	EXPECT_FALSE(assembler.accept(abandoned[1].header, abandoned[1].body).has_value());

	const auto next	   = makeBody(4000, 2);
	const auto message = feed(assembler, next);

	ASSERT_TRUE(message.has_value());
	EXPECT_EQ(message->body, next) << "Nothing of the abandoned message may leak into the next one";
}


TEST_F(MessageAssemblerTest, FragmentWithoutItsBeginning_IsDropped)
{
	const auto body	   = makeBody(3000);
	const auto packets = packetsOf(body);

	EXPECT_FALSE(assembler.accept(packets[1].header, packets[1].body).has_value());
	EXPECT_FALSE(assembler.accept(packets[2].header, packets[2].body).has_value()) << "Fragment 0 never arrived: nothing to complete";
	EXPECT_FALSE(assembler.isAssembling());
}


TEST_F(MessageAssemblerTest, SkippedFragment_AbortsTheMessage)
{
	const auto body	   = makeBody(4000);
	const auto packets = packetsOf(body);

	EXPECT_FALSE(assembler.accept(packets[0].header, packets[0].body).has_value());
	EXPECT_FALSE(assembler.accept(packets[2].header, packets[2].body).has_value()) << "A stream never skips: this is not the message in progress";
	EXPECT_FALSE(assembler.isAssembling());
	EXPECT_FALSE(assembler.accept(packets[3].header, packets[3].body).has_value()) << "The rest of the aborted message must not complete it";
}


TEST_F(MessageAssemblerTest, ShortNonLastFragment_IsRejected)
{
	const auto body	   = makeBody(4000);
	const auto packets = packetsOf(body);

	EXPECT_FALSE(assembler.accept(packets[0].header, packets[0].body.first(MaxFragmentBody - 1)).has_value());
	EXPECT_FALSE(assembler.isAssembling()) << "A sender fills every fragment but the last one";

	EXPECT_FALSE(assembler.accept(packets[0].header, packets[0].body).has_value());
	EXPECT_FALSE(assembler.accept(packets[1].header, packets[1].body.first(1)).has_value());
	EXPECT_FALSE(assembler.isAssembling()) << "Also in the middle of a message";

	const auto message = feed(assembler, body);
	ASSERT_TRUE(message.has_value()) << "The assembler keeps working after a dropped message";
	EXPECT_EQ(message->body, body);
}


TEST_F(MessageAssemblerTest, ForgedTotalLength_IsRejected)
{
	const auto	 body	 = makeBody(4000);
	auto		 packets = packetsOf(body);

	// The first fragment announces less than its fragments will carry
	PacketHeader forged	 = packets[0].header;
	forged.totalLength	 = static_cast<uint32_t>(3 * MaxFragmentBody + 10);
	ASSERT_EQ(fragmentsOf(forged.totalLength), forged.fragCount) << "The count still fits: only the last fragment can tell";

	EXPECT_FALSE(assembler.accept(forged, packets[0].body).has_value());
	EXPECT_FALSE(assembler.accept(packets[1].header, packets[1].body).has_value());
	EXPECT_FALSE(assembler.accept(packets[2].header, packets[2].body).has_value());
	EXPECT_FALSE(assembler.accept(packets[3].header, packets[3].body).has_value()) << "The message is not as long as it claimed to be";
	EXPECT_FALSE(assembler.isAssembling());

	// A count that does not belong to the announced length is not even started
	forged			 = packets[0].header;
	forged.fragCount = 9;
	EXPECT_FALSE(assembler.accept(forged, packets[0].body).has_value());
	EXPECT_FALSE(assembler.isAssembling());

	forged			   = packets[0].header;
	forged.totalLength = static_cast<uint32_t>(internal::MaxMessagePayload + 1);
	EXPECT_FALSE(assembler.accept(forged, packets[0].body).has_value());
	EXPECT_FALSE(assembler.isAssembling()) << "Larger than any message";
}


TEST_F(MessageAssemblerTest, Reset_ForgetsTheMessageInProgress)
{
	const auto body	   = makeBody(3000);
	const auto packets = packetsOf(body);
	assembler.accept(packets[0].header, packets[0].body);

	assembler.reset();

	EXPECT_FALSE(assembler.isAssembling());
	EXPECT_FALSE(assembler.accept(packets[1].header, packets[1].body).has_value());
	EXPECT_FALSE(assembler.accept(packets[2].header, packets[2].body).has_value()) << "Fragment 0 was forgotten";
}


TEST_F(MessageAssemblerTest, LargestMessageRoundTrip)
{
	const auto body	   = makeBody(internal::MaxMessagePayload);
	const auto message = feed(assembler, body, 77);

	ASSERT_TRUE(message.has_value());
	EXPECT_EQ(message->body, body);
	EXPECT_EQ(message->tag, 77u);
}


// ---------------------------------------------------------------------------
// Budget
// ---------------------------------------------------------------------------

TEST_F(MessageAssemblerTest, Budget_IsHeldWhileAMessageIsPutTogether)
{
	AssemblyBudget	 budget(10'000);
	MessageAssembler first(&budget);
	MessageAssembler second(&budget);

	const auto		 body	 = makeBody(6000);
	const auto		 packets = packetsOf(body);

	ASSERT_TRUE(first.hasRoomFor(packets[0].header));
	EXPECT_FALSE(first.accept(packets[0].header, packets[0].body).has_value());
	EXPECT_EQ(budget.used(), 6000u) << "The whole message is set aside with its first fragment";

	EXPECT_FALSE(second.hasRoomFor(packets[0].header)) << "No room for a second message of that size";
	EXPECT_TRUE(second.hasRoomFor(packets[1].header)) << "Only the beginning of a message can be refused";

	for (size_t i = 1; i < packets.size(); ++i)
		first.accept(packets[i].header, packets[i].body);

	EXPECT_EQ(budget.used(), 0u) << "A complete message leaves the budget";
	EXPECT_TRUE(second.hasRoomFor(packets[0].header));
}


TEST_F(MessageAssemblerTest, Budget_IsGivenBackWhenAMessageIsAbandoned)
{
	AssemblyBudget budget(10'000);
	const auto	   body	   = makeBody(6000);
	const auto	   packets = packetsOf(body);

	{
		MessageAssembler abandoned(&budget);
		abandoned.accept(packets[0].header, packets[0].body);
		EXPECT_EQ(budget.used(), 6000u);

		abandoned.reset();
		EXPECT_EQ(budget.used(), 0u);

		abandoned.accept(packets[0].header, packets[0].body);
	}

	EXPECT_EQ(budget.used(), 0u) << "A stream that goes away gives back what it held";
}


TEST_F(MessageAssemblerTest, UnfragmentedMessages_NeedNoBudget)
{
	AssemblyBudget	 budget(10);
	MessageAssembler small(&budget);

	const auto		 body	 = makeBody(1000);
	const auto		 packets = packetsOf(body);

	EXPECT_TRUE(small.hasRoomFor(packets[0].header));
	EXPECT_TRUE(small.accept(packets[0].header, packets[0].body).has_value());
}

} // namespace ChannelTests
