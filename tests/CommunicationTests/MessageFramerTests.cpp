#include <gtest/gtest.h>
#include <numeric>
#include <vector>

#include "Messaging/MessageFramer.h"

using namespace netlink;


namespace CommunicationTests
{

static InternalMessage makeMessage(uint32_t type, size_t size)
{
	InternalMessage message;
	message.type = type;
	message.data.resize(size);
	std::iota(message.data.begin(), message.data.end(), uint8_t{0});
	return message;
}


TEST(MessageFramer, HeaderIsBigEndian)
{
	InternalMessage message;
	message.type = 0x01020304;
	message.data = {0xAA, 0xBB};

	const auto frame = MessageFramer::serialize(message);

	ASSERT_EQ(frame.size(), MessageFramer::HeaderSize + 2);
	EXPECT_EQ(std::vector<uint8_t>(frame.begin(), frame.begin() + 8), (std::vector<uint8_t>{0x01, 0x02, 0x03, 0x04, 0x00, 0x00, 0x00, 0x02}))
		<< "The wire format must not depend on the host's byte order";
	EXPECT_EQ(frame[8], 0xAA);
	EXPECT_EQ(frame[9], 0xBB);
}


TEST(FrameDecoder, RoundTrip)
{
	const auto	 original = makeMessage(42, 100);

	FrameDecoder decoder;
	decoder.feed(MessageFramer::serialize(original));

	auto decoded = decoder.next();
	ASSERT_TRUE(decoded.has_value());
	EXPECT_EQ(decoded->type, original.type);
	EXPECT_EQ(decoded->data, original.data);
	EXPECT_FALSE(decoder.next().has_value());
	EXPECT_EQ(decoder.bufferedBytes(), 0u);
}


TEST(FrameDecoder, ZeroLengthPayload)
{
	FrameDecoder decoder;
	decoder.feed(MessageFramer::serialize(makeMessage(7, 0)));

	auto decoded = decoder.next();
	ASSERT_TRUE(decoded.has_value());
	EXPECT_EQ(decoded->type, 7u);
	EXPECT_TRUE(decoded->data.empty());
}


TEST(FrameDecoder, SeveralFramesInOneChunk)
{
	std::vector<uint8_t> stream;
	for (uint32_t i = 0; i < 5; ++i)
	{
		const auto frame = MessageFramer::serialize(makeMessage(i, i * 3));
		stream.insert(stream.end(), frame.begin(), frame.end());
	}

	FrameDecoder decoder;
	decoder.feed(stream);

	for (uint32_t i = 0; i < 5; ++i)
	{
		auto decoded = decoder.next();
		ASSERT_TRUE(decoded.has_value()) << "frame " << i;
		EXPECT_EQ(decoded->type, i);
		EXPECT_EQ(decoded->data.size(), i * 3);
	}

	EXPECT_FALSE(decoder.next().has_value());
}


TEST(FrameDecoder, FramesFedOneByteAtATime)
{
	std::vector<uint8_t> stream;
	for (uint32_t i = 0; i < 3; ++i)
	{
		const auto frame = MessageFramer::serialize(makeMessage(i + 1, 50));
		stream.insert(stream.end(), frame.begin(), frame.end());
	}

	FrameDecoder				 decoder;
	std::vector<InternalMessage> decoded;

	for (uint8_t byte : stream)
	{
		decoder.feed(std::span<const uint8_t>(&byte, 1));
		while (auto message = decoder.next())
			decoded.push_back(std::move(*message));
	}

	ASSERT_EQ(decoded.size(), 3u);
	for (uint32_t i = 0; i < 3; ++i)
	{
		EXPECT_EQ(decoded[i].type, i + 1);
		EXPECT_EQ(decoded[i].data, makeMessage(0, 50).data);
	}
}


TEST(FrameDecoder, PartialHeaderWaitsForMoreBytes)
{
	const auto	 frame = MessageFramer::serialize(makeMessage(1, 10));

	FrameDecoder decoder;
	decoder.feed(std::span<const uint8_t>(frame.data(), 5));

	EXPECT_FALSE(decoder.next().has_value());
	EXPECT_FALSE(decoder.hasError());

	decoder.feed(std::span<const uint8_t>(frame.data() + 5, frame.size() - 5));
	EXPECT_TRUE(decoder.next().has_value());
}


TEST(FrameDecoder, OversizedLengthIsAProtocolError)
{
	FrameDecoder		 decoder(1024);

	std::vector<uint8_t> header(MessageFramer::HeaderSize);
	MessageFramer::writeUint32(header.data(), 1);
	MessageFramer::writeUint32(header.data() + 4, 0xFFFFFFFF);

	decoder.feed(header);

	EXPECT_FALSE(decoder.next().has_value());
	EXPECT_TRUE(decoder.hasError()) << "A length above the maximum must be rejected instead of buffering gigabytes";

	decoder.feed(MessageFramer::serialize(makeMessage(2, 1)));
	EXPECT_FALSE(decoder.next().has_value()) << "A corrupt stream cannot be resynchronized and must stay failed";
}

} // namespace CommunicationTests
