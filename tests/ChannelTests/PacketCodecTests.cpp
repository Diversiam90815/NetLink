#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "Channel/Protocol/AckRanges.h"
#include "Channel/Protocol/PacketHeader.h"

using namespace netlink;
using namespace netlink::channel;


namespace ChannelTests
{

static PacketHeader makeDataHeader(uint64_t seq, Lane lane = Lane::Reliable)
{
	PacketHeader header;
	header.flags	   = PacketFlags::data(lane);
	header.srcStreamID = 0xA1B2C3D4;
	header.dstStreamID = 0x01020304;
	header.seq		   = seq;
	return header;
}

// The header of fragment `index` of a message of that length
static PacketHeader makeFragmentHeader(uint32_t totalLength, uint16_t index, Lane lane = Lane::Reliable)
{
	PacketHeader header = makeDataHeader(10 + index, lane);
	header.fragCount	= static_cast<uint16_t>(fragmentsOf(totalLength));
	header.fragIndex	= index;
	header.totalLength	= totalLength;
	header.flags.setFragment(true, index + 1 == header.fragCount);
	return header;
}

static std::vector<uint8_t> fullFragment()
{
	return std::vector<uint8_t>(MaxFragmentBody, 0xAB);
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
	PacketHeader header = makeFragmentHeader(5000, 0);
	header.tag			= 5;

	std::array<uint8_t, MaxHeaderSize> head{};
	const size_t					   size = encodeHeader(header, head.data());

	EXPECT_EQ(size, header.encodedSize());
	EXPECT_EQ(size, MaxHeaderSize) << "The first fragment of a message carries every extension";
	EXPECT_EQ(std::vector<uint8_t>(head.begin(), head.begin() + size), encodePacket(header)) << "A header sent in front of a separate body is the same header";
}


TEST(PacketCodec, WireLayoutIsBigEndian)
{
	const auto datagram = encodePacket(makeDataHeader(0x0102030405060708ull));

	EXPECT_EQ(datagram[0], 0x4E);
	EXPECT_EQ(datagram[1], 0x4C);
	EXPECT_EQ(datagram[2], 3) << "Protocol version 3";
	EXPECT_EQ(datagram[3], 0x04) << "Data on the Reliable lane";
	EXPECT_EQ(datagram[4], 0xA1);
	EXPECT_EQ(datagram[7], 0xD4);
	EXPECT_EQ(datagram[12], 0x01);
	EXPECT_EQ(datagram[19], 0x08);
}


TEST(PacketCodec, FirstFragment_CarriesTagAndTotalLength)
{
	PacketHeader header = makeFragmentHeader(5000, 0);
	header.tag			= 0x01020304;

	const auto datagram = encodePacket(header, fullFragment());
	ASSERT_EQ(datagram.size(), internal::MaxDatagramSize) << "A full fragment behind the largest header fills the datagram exactly";

	auto decoded = decodePacket(datagram);
	ASSERT_TRUE(decoded.has_value());
	EXPECT_EQ(decoded->header.tag, 0x01020304u);
	EXPECT_EQ(decoded->header.totalLength, 5000u);
	EXPECT_EQ(decoded->header.fragCount, 5);
	EXPECT_EQ(decoded->body.size(), MaxFragmentBody);
	EXPECT_EQ(decoded->body.front(), 0xAB) << "The body starts behind the extensions";
}


TEST(PacketCodec, LaterFragments_CarryOnlyTheirPosition)
{
	PacketHeader header = makeFragmentHeader(5000, 3);
	header.tag			= 123; // not on the wire

	const auto datagram = encodePacket(header, fullFragment());
	ASSERT_EQ(datagram.size(), BaseHeaderSize + FragmentExtensionSize + MaxFragmentBody);

	auto decoded = decodePacket(datagram);
	ASSERT_TRUE(decoded.has_value());
	EXPECT_TRUE(decoded->header.flags.isFragmented());
	EXPECT_EQ(decoded->header.fragIndex, 3);
	EXPECT_EQ(decoded->header.fragCount, 5);
	EXPECT_EQ(decoded->header.tag, 0u);
	EXPECT_EQ(decoded->header.totalLength, 0u);
}


TEST(PacketCodec, AckAndPing_CarryTheirLaneWithoutATag)
{
	PacketHeader ack;
	ack.flags		   = PacketFlags::ack(Lane::Bulk, true);
	ack.srcStreamID	   = 7;
	ack.seq			   = 99;
	ack.tag			   = 5; // not on the wire: an Ack starts no message

	const auto body	   = encodeAck({.serial = 1, .mediaReceived = 2, .ranges = {{101, 3}}}, 10);
	auto	   decoded = decodePacket(encodePacket(ack, body));
	ASSERT_TRUE(decoded.has_value());
	EXPECT_EQ(decoded->header.flags.kind(), PacketKind::Ack);
	EXPECT_EQ(decoded->header.flags.lane(), Lane::Bulk) << "Every lane is acknowledged on its own";
	EXPECT_TRUE(decoded->header.flags.isPaused());
	EXPECT_EQ(decoded->header.seq, 99u);
	EXPECT_EQ(decoded->header.tag, 0u);
	EXPECT_EQ(decoded->body.size(), body.size());

	PacketHeader ping;
	ping.flags				= PacketFlags::ping(Lane::Control);
	ping.srcStreamID		= 7;

	const auto pingDatagram = encodePacket(ping);
	EXPECT_EQ(pingDatagram.size(), BaseHeaderSize);
	EXPECT_TRUE(decodePacket(pingDatagram).has_value());

	EXPECT_FALSE(decodePacket(encodePacket(ping, std::vector<uint8_t>{1})).has_value()) << "A Ping has no body";
}


TEST(PacketCodec, RejectsForeignOrBrokenDatagrams)
{
	auto valid = encodePacket(makeDataHeader(1), std::vector<uint8_t>{1});

	EXPECT_FALSE(decodePacket(std::span(valid.data(), BaseHeaderSize - 1)).has_value()) << "Truncated header";

	auto badMagic = valid;
	badMagic[0]	  = 'X';
	EXPECT_FALSE(decodePacket(badMagic).has_value()) << "Not our protocol";

	auto newerVersion = valid;
	newerVersion[2]	  = ProtocolVersion + 1;
	EXPECT_FALSE(decodePacket(newerVersion).has_value());

	auto olderVersion = valid;
	olderVersion[2]	  = ProtocolVersion - 1;
	EXPECT_FALSE(decodePacket(olderVersion).has_value()) << "Builds speaking an older wire format must not be understood by accident";

	auto noTag = encodePacket(makeDataHeader(1));
	EXPECT_FALSE(decodePacket(std::span(noTag.data(), BaseHeaderSize + 2)).has_value()) << "A Data packet that starts a message without its whole tag";

	auto encrypted = valid;
	encrypted[3] |= 0x80;
	EXPECT_FALSE(decodePacket(encrypted).has_value());

	PacketHeader noStreamID = makeDataHeader(1);
	noStreamID.srcStreamID	= 0;
	EXPECT_FALSE(decodePacket(encodePacket(noStreamID)).has_value());

	EXPECT_FALSE(decodePacket(encodePacket(makeDataHeader(0))).has_value()) << "Data seqs start at 1";

	PacketHeader beacon;
	beacon.flags	   = PacketFlags{}.setKind(PacketKind::Beacon);
	beacon.srcStreamID = 7;
	EXPECT_FALSE(decodePacket(encodePacket(beacon)).has_value()) << "A beacon is not a packet of a link";
}


TEST(PacketCodec, OversizedDatagram_IsDropped)
{
	const PacketHeader		   header = makeDataHeader(1);

	const std::vector<uint8_t> fits(MaxFragmentBody);
	EXPECT_TRUE(decodePacket(encodePacket(header, fits)).has_value()) << "The largest body a link sends in one piece";

	const std::vector<uint8_t> tooLong(MaxFragmentBody + 1);
	EXPECT_FALSE(decodePacket(encodePacket(header, tooLong)).has_value()) << "A sender would have fragmented it";

	const std::vector<uint8_t> tooLarge(internal::MaxDatagramSize);
	EXPECT_FALSE(decodePacket(encodePacket(header, tooLarge)).has_value()) << "More than any link puts into a datagram";
}


TEST(PacketCodec, FragmentCount_MustMatchTheTotalLength)
{
	PacketHeader header = makeFragmentHeader(5000, 0);
	EXPECT_TRUE(decodePacket(encodePacket(header, fullFragment())).has_value());

	header.fragCount = 6;
	EXPECT_FALSE(decodePacket(encodePacket(header, fullFragment())).has_value()) << "5000 bytes are 5 fragments, not 6";

	header			   = makeFragmentHeader(5000, 0);
	header.totalLength = static_cast<uint32_t>(internal::MaxMessagePayload + 1);
	header.fragCount   = static_cast<uint16_t>(fragmentsOf(header.totalLength));
	EXPECT_FALSE(decodePacket(encodePacket(header, fullFragment())).has_value()) << "Larger than the largest message";

	// The largest message there is
	header = makeFragmentHeader(static_cast<uint32_t>(internal::MaxMessagePayload), 0);
	EXPECT_TRUE(decodePacket(encodePacket(header, fullFragment())).has_value());

	// Later fragments carry no length, but no lane has more fragments than its largest message needs
	header			 = makeFragmentHeader(5000, 1);
	header.fragCount = static_cast<uint16_t>(fragmentsOf(internal::MaxMessagePayload) + 1);
	EXPECT_FALSE(decodePacket(encodePacket(header, fullFragment())).has_value());
}


TEST(PacketCodec, MediaMessages_AreLimitedToTheirOwnSize)
{
	PacketHeader header = makeFragmentHeader(static_cast<uint32_t>(internal::MaxMediaPayload), 0, Lane::Media);
	EXPECT_TRUE(decodePacket(encodePacket(header, fullFragment())).has_value());

	header = makeFragmentHeader(static_cast<uint32_t>(internal::MaxMediaPayload + 1), 0, Lane::Media);
	EXPECT_FALSE(decodePacket(encodePacket(header, fullFragment())).has_value()) << "Fine on a reliable lane, too large for Media";
}


TEST(PacketCodec, RejectsInconsistentFragments)
{
	PacketHeader			   header = makeFragmentHeader(5000, 4);
	const std::vector<uint8_t> tail(5000 - 4 * MaxFragmentBody);
	EXPECT_TRUE(decodePacket(encodePacket(header, tail)).has_value());

	header.flags.setFragment(true, false);
	EXPECT_FALSE(decodePacket(encodePacket(header, tail)).has_value()) << "Index 4 of 5 is the last one, but the flag says otherwise";

	header			 = makeFragmentHeader(5000, 4);
	header.fragIndex = 5;
	EXPECT_FALSE(decodePacket(encodePacket(header, tail)).has_value()) << "Index out of range";

	header = makeFragmentHeader(5000, 2);
	EXPECT_FALSE(decodePacket(encodePacket(header, tail)).has_value()) << "Every fragment but the last one is full";

	header			 = makeFragmentHeader(5000, 0);
	header.fragCount = 1;
	header.flags.setFragment(true, true);
	EXPECT_FALSE(decodePacket(encodePacket(header, tail)).has_value()) << "A message of one fragment is not fragmented";

	auto truncated = encodePacket(makeFragmentHeader(5000, 2), fullFragment());
	EXPECT_FALSE(decodePacket(std::span(truncated.data(), BaseHeaderSize + 2)).has_value()) << "Fragment flag without the extension";
}

} // namespace ChannelTests
