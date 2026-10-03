#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "Channel/Protocol/PacketHeader.h"

using namespace netlink::channel;


namespace ChannelTests
{

static PacketHeader makeDataHeader(uint64_t seq)
{
	PacketHeader header;
	header.flags	   = PacketFlags::data(ChannelId::Application, true);
	header.srcStreamID = 0xA1B2C3D4;
	header.dstStreamID = 0x01020304;
	header.seq		   = seq;
	return header;
}


TEST(PacketCodec, UnfragmentedRoundTrip)
{
	const std::vector<uint8_t> body{1, 2, 3, 4, 5};
	PacketHeader			   header = makeDataHeader(0x0102030405060708ull);
	header.tag						  = 0xCAFE0001;

	const auto datagram = encodePacket(header, body);
	ASSERT_EQ(datagram.size(), BaseHeaderSize + TagExtensionSize + body.size()) << "An unfragmented Data packet starts a message, so it carries the tag";

	auto decoded = decodePacket(datagram);
	ASSERT_TRUE(decoded.has_value());

	EXPECT_EQ(decoded->header.flags, header.flags);
	EXPECT_EQ(decoded->header.srcStreamID, header.srcStreamID);
	EXPECT_EQ(decoded->header.dstStreamID, header.dstStreamID);
	EXPECT_EQ(decoded->header.seq, header.seq) << "The full 64-bit key must survive the wire";
	EXPECT_EQ(decoded->header.tag, 0xCAFE0001u);
	EXPECT_EQ(std::vector<uint8_t>(decoded->body.begin(), decoded->body.end()), body);
}


TEST(PacketCodec, EncodeHeader_MatchesEncodePacket)
{
	PacketHeader header = makeDataHeader(77);
	header.tag			= 5;
	header.flags.setFragment(true, false);
	header.fragIndex = 0;
	header.fragCount = 3;

	std::array<uint8_t, MaxHeaderSize> head{};
	const size_t					   size = encodeHeader(header, head.data());

	EXPECT_EQ(size, header.encodedSize());
	EXPECT_EQ(size, MaxHeaderSize) << "The first fragment of a message carries both extensions";
	EXPECT_EQ(std::vector<uint8_t>(head.begin(), head.begin() + size), encodePacket(header)) << "A header sent in front of a separate body is the same header";
}


TEST(PacketCodec, WireLayoutIsBigEndian)
{
	const auto datagram = encodePacket(makeDataHeader(0x0102030405060708ull));

	EXPECT_EQ(datagram[0], 0x4E);
	EXPECT_EQ(datagram[1], 0x4C);
	EXPECT_EQ(datagram[2], ProtocolVersion);
	EXPECT_EQ(datagram[4], 0xA1);
	EXPECT_EQ(datagram[7], 0xD4);
	EXPECT_EQ(datagram[12], 0x01);
	EXPECT_EQ(datagram[19], 0x08);
}


TEST(PacketCodec, FragmentExtensionRoundTrip)
{
	PacketHeader header = makeDataHeader(42);
	header.flags.setFragment(true, false);
	header.fragIndex = 3;
	header.fragCount = 7;

	header.tag = 123; // not on the wire: only the first fragment carries it

	const std::vector<uint8_t> body(100, 0xAB);
	const auto				   datagram = encodePacket(header, body);
	ASSERT_EQ(datagram.size(), BaseHeaderSize + FragmentExtensionSize + body.size());

	auto decoded = decodePacket(datagram);
	ASSERT_TRUE(decoded.has_value());
	EXPECT_TRUE(decoded->header.flags.isFragmented());
	EXPECT_EQ(decoded->header.fragIndex, 3);
	EXPECT_EQ(decoded->header.fragCount, 7);
	EXPECT_EQ(decoded->header.tag, 0u);
	EXPECT_EQ(decoded->body.size(), body.size());
}


TEST(PacketCodec, FirstFragmentCarriesTheTag)
{
	PacketHeader header = makeDataHeader(42);
	header.flags.setFragment(true, false);
	header.fragIndex = 0;
	header.fragCount = 7;
	header.tag		 = 0x01020304;

	const std::vector<uint8_t> body(100, 0xAB);
	const auto				   datagram = encodePacket(header, body);
	ASSERT_EQ(datagram.size(), BaseHeaderSize + FragmentExtensionSize + TagExtensionSize + body.size());

	auto decoded = decodePacket(datagram);
	ASSERT_TRUE(decoded.has_value());
	EXPECT_EQ(decoded->header.tag, 0x01020304u);
	EXPECT_EQ(decoded->body.size(), body.size());
	EXPECT_EQ(decoded->body.front(), 0xAB) << "The body starts behind the tag";
}


TEST(PacketCodec, UnreliableDataCarriesTheTag)
{
	PacketHeader header;
	header.flags	   = PacketFlags::data(ChannelId::Application, false);
	header.srcStreamID = 7;
	header.seq		   = 3;
	header.tag		   = 99;

	auto decoded	   = decodePacket(encodePacket(header, std::vector<uint8_t>{1}));
	ASSERT_TRUE(decoded.has_value());
	EXPECT_FALSE(decoded->header.flags.isReliable());
	EXPECT_EQ(decoded->header.tag, 99u);
	EXPECT_EQ(decoded->body.size(), 1u);
}


TEST(PacketCodec, AcksCarryTheirSeqAndChannelWithoutATag)
{
	for (const PacketKind kind : {PacketKind::DataAck, PacketKind::AckAck})
	{
		PacketHeader header;
		header.flags	   = PacketFlags::ack(kind, ChannelId::Application);
		header.srcStreamID = 7;
		header.seq		   = 99;
		header.tag		   = 5; // not on the wire: acknowledgements start no message

		const auto datagram = encodePacket(header);
		EXPECT_EQ(datagram.size(), BaseHeaderSize);

		auto decoded = decodePacket(datagram);
		ASSERT_TRUE(decoded.has_value());
		EXPECT_EQ(decoded->header.flags.kind(), kind);
		EXPECT_EQ(decoded->header.flags.channel(), ChannelId::Application) << "Every channel is acknowledged on its own";
		EXPECT_EQ(decoded->header.srcStreamID, 7u);
		EXPECT_EQ(decoded->header.seq, 99u);
		EXPECT_EQ(decoded->header.tag, 0u);
		EXPECT_TRUE(decoded->body.empty());
	}
}


TEST(PacketCodec, RejectsForeignOrBrokenDatagrams)
{
	auto valid = encodePacket(makeDataHeader(1), std::vector<uint8_t>{1});

	EXPECT_FALSE(decodePacket(std::span(valid.data(), BaseHeaderSize - 1)).has_value()) << "Truncated header";

	auto badMagic = valid;
	badMagic[0]	  = 'X';
	EXPECT_FALSE(decodePacket(badMagic).has_value()) << "Not our protocol (e.g. a JSON datagram of an older build)";

	auto badVersion = valid;
	badVersion[2]	= ProtocolVersion + 1;
	EXPECT_FALSE(decodePacket(badVersion).has_value());

	auto olderVersion = valid;
	olderVersion[2]	  = ProtocolVersion - 1;
	EXPECT_FALSE(decodePacket(olderVersion).has_value()) << "Builds speaking an older wire format must not be understood by accident";

	auto noTag = encodePacket(makeDataHeader(1));
	EXPECT_FALSE(decodePacket(std::span(noTag.data(), BaseHeaderSize + 2)).has_value()) << "A Data packet that starts a message without its whole tag";

	auto reservedFlags = valid;
	reservedFlags[3] |= 0x80;
	EXPECT_FALSE(decodePacket(reservedFlags).has_value());

	PacketHeader noStreamID = makeDataHeader(1);
	noStreamID.srcStreamID	= 0;
	EXPECT_FALSE(decodePacket(encodePacket(noStreamID)).has_value());
}


TEST(PacketCodec, OversizedDatagram_IsDropped)
{
	const PacketHeader		   header = makeDataHeader(1);

	const std::vector<uint8_t> fits(netlink::internal::MaxDatagramSize - header.encodedSize());
	EXPECT_TRUE(decodePacket(encodePacket(header, fits)).has_value()) << "A full datagram, the largest one a link sends";

	const std::vector<uint8_t> tooLarge(fits.size() + 1);
	EXPECT_FALSE(decodePacket(encodePacket(header, tooLarge)).has_value()) << "One byte more than any link sends";
}


TEST(PacketCodec, FragmentCountBeyondTheLargestMessage_IsRejected)
{
	PacketHeader header = makeDataHeader(5);
	header.flags.setFragment(true, false);
	header.fragIndex = 0;

	header.fragCount = static_cast<uint16_t>(MaxFragmentCount);
	EXPECT_TRUE(decodePacket(encodePacket(header)).has_value()) << "The fragments of a 16 MiB message";

	header.fragCount = static_cast<uint16_t>(MaxFragmentCount + 1);
	EXPECT_FALSE(decodePacket(encodePacket(header)).has_value());

	header.fragCount = UINT16_MAX;
	EXPECT_FALSE(decodePacket(encodePacket(header)).has_value());
}


TEST(PacketCodec, RejectsInconsistentFragmentExtension)
{
	PacketHeader header = makeDataHeader(5);
	header.flags.setFragment(true, false);
	header.fragIndex = 1;
	header.fragCount = 2; // index 1 of 2 is the last one, but the flag says otherwise

	EXPECT_FALSE(decodePacket(encodePacket(header)).has_value());

	header.flags.setFragment(true, true);
	EXPECT_TRUE(decodePacket(encodePacket(header)).has_value());

	header.fragIndex = 2; // out of range
	EXPECT_FALSE(decodePacket(encodePacket(header)).has_value());

	auto truncated = encodePacket(header);
	EXPECT_FALSE(decodePacket(std::span(truncated.data(), BaseHeaderSize + 2)).has_value()) << "Fragment flag without the extension";
}

} // namespace ChannelTests
