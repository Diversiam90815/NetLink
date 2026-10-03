#include <gtest/gtest.h>

#include <optional>
#include <vector>

#include "Channel/Fragmentation/FragmentationService.h"
#include "Channel/Fragmentation/MessageAssembler.h"

using namespace netlink;
using namespace netlink::channel;


namespace ChannelTests
{

class MessageAssemblerTest : public ::testing::Test
{
protected:
	static constexpr size_t		MaxBody = 100;

	// Fragments of these tests are cut small
	MessageAssembler			assembler{internal::MaxMessagePayload, MaxBody};

	static std::vector<uint8_t> makeBody(size_t size, uint8_t seed = 7)
	{
		std::vector<uint8_t> body(size);
		for (size_t i = 0; i < size; ++i)
			body[i] = static_cast<uint8_t>(i * 31 + seed);
		return body;
	}

	// Header the link would put on the given fragment
	static PacketHeader headerFor(const Fragment &fragment, uint32_t tag = 0)
	{
		PacketHeader header;
		header.flags	   = PacketFlags::data(ChannelId::Application, true);
		header.srcStreamID = 1;
		header.seq		   = 1 + fragment.index;
		header.tag		   = tag;

		if (fragment.isFragmented())
		{
			header.flags.setFragment(true, fragment.isLast());
			header.fragIndex = fragment.index;
			header.fragCount = fragment.count;
		}

		return header;
	}

	// Feeds the message the way a stream delivers it: every fragment, in order
	std::optional<AssembledMessage> feed(MessageAssembler &target, const std::vector<uint8_t> &body, uint32_t tag = 0, size_t maxBody = MaxBody)
	{
		std::optional<AssembledMessage> result;

		for (const auto &fragment : FragmentationService::split(body, maxBody))
		{
			if (auto message = target.accept(headerFor(fragment, tag), fragment.body))
			{
				EXPECT_FALSE(result.has_value()) << "A message must complete exactly once";
				result = std::move(message);
			}
		}

		return result;
	}
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
	const auto body		 = makeBody(1050);
	const auto fragments = FragmentationService::split(body, MaxBody);

	for (size_t i = 0; i + 1 < fragments.size(); ++i)
	{
		EXPECT_FALSE(assembler.accept(headerFor(fragments[i], 9), fragments[i].body).has_value()) << "Not complete before the last fragment";
		EXPECT_TRUE(assembler.isAssembling());
	}

	const auto message = assembler.accept(headerFor(fragments.back(), 9), fragments.back().body);

	ASSERT_TRUE(message.has_value());
	EXPECT_EQ(message->body, body);
	EXPECT_EQ(message->tag, 9u) << "The tag comes with the first fragment";
	EXPECT_FALSE(assembler.isAssembling()) << "Completed messages must not leave state behind";
}


TEST_F(MessageAssemblerTest, ConsecutiveMessages_StaySeparate)
{
	const auto first  = makeBody(250, 1);
	const auto second = makeBody(30, 2);
	const auto third  = makeBody(420, 3);

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
	// The sender dropped the rest of a message (session closed) and went on with the next one
	const auto abandoned = FragmentationService::split(makeBody(250, 1), MaxBody);
	EXPECT_FALSE(assembler.accept(headerFor(abandoned[0]), abandoned[0].body).has_value());
	EXPECT_FALSE(assembler.accept(headerFor(abandoned[1]), abandoned[1].body).has_value());

	const auto next	   = makeBody(250, 2);
	const auto message = feed(assembler, next);

	ASSERT_TRUE(message.has_value());
	EXPECT_EQ(message->body, next) << "Nothing of the abandoned message may leak into the next one";
}


TEST_F(MessageAssemblerTest, AbandonedMessage_IsReplacedByAnUnfragmentedOne)
{
	const auto abandoned = FragmentationService::split(makeBody(250, 1), MaxBody);
	EXPECT_FALSE(assembler.accept(headerFor(abandoned[0]), abandoned[0].body).has_value());

	const auto next	   = makeBody(10, 2);
	const auto message = feed(assembler, next);

	ASSERT_TRUE(message.has_value());
	EXPECT_EQ(message->body, next);
	EXPECT_FALSE(assembler.isAssembling());
}


TEST_F(MessageAssemblerTest, FragmentWithoutItsBeginning_IsDropped)
{
	const auto fragments = FragmentationService::split(makeBody(250), MaxBody);

	EXPECT_FALSE(assembler.accept(headerFor(fragments[1]), fragments[1].body).has_value());
	EXPECT_FALSE(assembler.accept(headerFor(fragments[2]), fragments[2].body).has_value()) << "Fragment 0 never arrived: nothing to complete";
	EXPECT_FALSE(assembler.isAssembling());
}


TEST_F(MessageAssemblerTest, InconsistentFragment_AbortsTheMessage)
{
	const auto fragments = FragmentationService::split(makeBody(350), MaxBody);

	EXPECT_FALSE(assembler.accept(headerFor(fragments[0]), fragments[0].body).has_value());

	PacketHeader bogus = headerFor(fragments[1]);
	bogus.fragCount	   = 9;
	EXPECT_FALSE(assembler.accept(bogus, fragments[1].body).has_value());
	EXPECT_FALSE(assembler.isAssembling()) << "A contradicting fragment abandons the message";

	EXPECT_FALSE(assembler.accept(headerFor(fragments[2]), fragments[2].body).has_value());
	EXPECT_FALSE(assembler.accept(headerFor(fragments[3]), fragments[3].body).has_value()) << "The rest of the aborted message must not complete it";
}


TEST_F(MessageAssemblerTest, SkippedFragment_AbortsTheMessage)
{
	const auto fragments = FragmentationService::split(makeBody(350), MaxBody);

	EXPECT_FALSE(assembler.accept(headerFor(fragments[0]), fragments[0].body).has_value());
	EXPECT_FALSE(assembler.accept(headerFor(fragments[2]), fragments[2].body).has_value()) << "A stream never skips: this is not the message in progress";
	EXPECT_FALSE(assembler.isAssembling());
}


TEST_F(MessageAssemblerTest, OversizedMessage_IsDropped)
{
	MessageAssembler small(150, MaxBody);

	EXPECT_FALSE(feed(small, makeBody(250)).has_value());
	EXPECT_FALSE(small.isAssembling());

	EXPECT_FALSE(feed(small, makeBody(151)).has_value()) << "Also when it fits into one packet";

	const auto fits = makeBody(150);
	const auto message = feed(small, fits);
	ASSERT_TRUE(message.has_value()) << "The limit itself is allowed, and the assembler keeps working after a dropped message";
	EXPECT_EQ(message->body, fits);
}


TEST_F(MessageAssemblerTest, Reset_ForgetsTheMessageInProgress)
{
	const auto fragments = FragmentationService::split(makeBody(250), MaxBody);
	assembler.accept(headerFor(fragments[0]), fragments[0].body);

	assembler.reset();

	EXPECT_FALSE(assembler.isAssembling());
	EXPECT_FALSE(assembler.accept(headerFor(fragments[1]), fragments[1].body).has_value());
	EXPECT_FALSE(assembler.accept(headerFor(fragments[2]), fragments[2].body).has_value()) << "Fragment 0 was forgotten";
}


TEST_F(MessageAssemblerTest, ShortNonLastFragment_IsRejected)
{
	const auto fragments = FragmentationService::split(makeBody(350), MaxBody);

	EXPECT_FALSE(assembler.accept(headerFor(fragments[0]), fragments[0].body.first(MaxBody - 1)).has_value());
	EXPECT_FALSE(assembler.isAssembling()) << "A sender fills every fragment but the last one";

	EXPECT_FALSE(assembler.accept(headerFor(fragments[0]), fragments[0].body).has_value());
	EXPECT_FALSE(assembler.accept(headerFor(fragments[1]), fragments[1].body.first(1)).has_value());
	EXPECT_FALSE(assembler.isAssembling()) << "Also in the middle of a message";

	EXPECT_FALSE(assembler.accept(headerFor(fragments[2]), fragments[2].body).has_value());
	EXPECT_FALSE(assembler.accept(headerFor(fragments[3]), fragments[3].body).has_value()) << "The rest of the dropped message must not complete it";

	const auto body	   = makeBody(350, 2);
	const auto message = feed(assembler, body);
	ASSERT_TRUE(message.has_value()) << "The last fragment may be shorter, and the assembler keeps working after a dropped message";
	EXPECT_EQ(message->body, body);
}


TEST_F(MessageAssemblerTest, ForgedFragmentCount_DoesNotReserveTheMaximum)
{
	MessageAssembler		   wire; // fragments as a link really cuts them
	const std::vector<uint8_t> body(MaxFragmentBody);

	// The first fragment of what claims to be the largest message there is
	PacketHeader			   header;
	header.flags	   = PacketFlags::data(ChannelId::Application, true).setFragment(true, false);
	header.srcStreamID = 1;
	header.seq		   = 1;
	header.fragIndex   = 0;
	header.fragCount   = static_cast<uint16_t>(MaxFragmentCount);

	EXPECT_FALSE(wire.accept(header, body).has_value());
	ASSERT_TRUE(wire.isAssembling());
	EXPECT_LE(wire.reservedBytes(), MessageAssembler::MaxInitialReserve) << "One datagram must not make the receiver set 16 MiB aside";
}


TEST_F(MessageAssemblerTest, LargeMessageRoundTrip)
{
	MessageAssembler wire; // fragments as a link really cuts them

	const auto		 body	 = makeBody(size_t{4} * 1024 * 1024);
	const auto		 message = feed(wire, body, 77, MaxFragmentBody);

	ASSERT_TRUE(message.has_value()) << "Also when the message outgrows what was set aside for it at first";
	EXPECT_EQ(message->body, body);
	EXPECT_EQ(message->tag, 77u);
}

} // namespace ChannelTests
