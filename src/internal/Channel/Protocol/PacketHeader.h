/*
  ==============================================================================
	Module:         PacketHeader
	Description:    Header of every datagram of a link and its binary encoding (network byte order)
  ==============================================================================
*/

#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "ByteOrder.h"
#include "PacketFlags.h"
#include "TransportConstants.h"


namespace netlink::channel
{

inline constexpr uint16_t ProtocolMagic			= 0x4E4C;
inline constexpr uint8_t  ProtocolVersion		= 3;
inline constexpr size_t	  BaseHeaderSize		= 20;
inline constexpr size_t	  FragmentExtensionSize = 4;
inline constexpr size_t	  TagExtensionSize		= 4;
inline constexpr size_t	  LengthExtensionSize	= 4;
inline constexpr size_t	  MaxHeaderSize			= BaseHeaderSize + FragmentExtensionSize + TagExtensionSize + LengthExtensionSize;

// Every fragment of a message but the last one carries exactly this many bytes
inline constexpr size_t	  MaxFragmentBody		= internal::MaxDatagramSize - MaxHeaderSize;

constexpr size_t		  fragmentsOf(const size_t length)
{
	return length == 0 ? 1 : (length + MaxFragmentBody - 1) / MaxFragmentBody;
}

// The largest message a lane carries
constexpr size_t maxMessageSize(const Lane lane)
{
	return lane == Lane::Media ? internal::MaxMediaPayload : internal::MaxMessagePayload;
}


/*
 Packet header encoding:

	0   u16   magic (0x4E4C = "NL")
	2   u8    version
	3   u8    flags
	4   u32   srcStreamID
	8   u32   dstStreamID       (0 = sender doesn't know the receiver's stream ID yet)
	12  u64   seq               → offset 12 + 8 bytes = 20 (matches BaseHeaderSize)

	[only if flags.isFragmented()]
		u16   fragIndex
		u16   fragCount

	[only on a Data packet that starts a message: unfragmented, or fragIndex 0]
		u32   tag               (opaque to the channel: the message type of the layer above)

	[only on fragment 0]
		u32   totalLength       (of the whole message)

 seq per packet kind (every lane counts for itself):
	Data		seq of this packet. On the Media lane: of this datagram, the message is seq - fragIndex
	Ack			highest seq received without a gap; the body lists what waits behind a gap (see AckRanges.h)
	Ping		unused (0)
 */
struct PacketHeader
{
	PacketFlags flags{};
	uint32_t	srcStreamID{0}; // random ID of the sender's current stream; changes when that stream restarts
	uint32_t	dstStreamID{0}; // 0 while the sender does not know the receiver yet
	uint64_t	seq{0};
	uint16_t	fragIndex{0}; // only on the wire when flags.isFragmented()
	uint16_t	fragCount{0};
	uint32_t	tag{0};		  // only on the wire when startsMessage()
	uint32_t	totalLength{0}; // only on the wire when startsFragmentedMessage()

	// The packet carries the beginning of a message, and with it the message's tag
	bool		startsMessage() const { return flags.kind() == PacketKind::Data && (!flags.isFragmented() || fragIndex == 0); }
	bool		startsFragmentedMessage() const { return flags.isFragmented() && fragIndex == 0; }

	size_t		encodedSize() const
	{
		return BaseHeaderSize + (flags.isFragmented() ? FragmentExtensionSize : 0) + (startsMessage() ? TagExtensionSize : 0) +
			   (startsFragmentedMessage() ? LengthExtensionSize : 0);
	}
};


struct DecodedPacket
{
	PacketHeader			 header;
	std::span<const uint8_t> body; // points into the decoded datagram
};


// Writes the header into out, which must hold MaxHeaderSize bytes. Returns the number of bytes written.
inline size_t encodeHeader(const PacketHeader &header, uint8_t *out)
{
	writeUint16(out, ProtocolMagic);
	out[2] = ProtocolVersion;
	out[3] = header.flags.raw();
	writeUint32(out + 4, header.srcStreamID);
	writeUint32(out + 8, header.dstStreamID);
	writeUint64(out + 12, header.seq);

	size_t size = BaseHeaderSize;

	if (header.flags.isFragmented())
	{
		writeUint16(out + size, header.fragIndex);
		writeUint16(out + size + 2, header.fragCount);
		size += FragmentExtensionSize;
	}

	if (header.startsMessage())
	{
		writeUint32(out + size, header.tag);
		size += TagExtensionSize;
	}

	if (header.startsFragmentedMessage())
	{
		writeUint32(out + size, header.totalLength);
		size += LengthExtensionSize;
	}

	return size;
}


inline std::vector<uint8_t> encodePacket(const PacketHeader &header, std::span<const uint8_t> body = {})
{
	std::vector<uint8_t> datagram(header.encodedSize() + body.size());
	const size_t		 headerSize = encodeHeader(header, datagram.data());

	if (!body.empty())
		std::ranges::copy(body, datagram.begin() + static_cast<std::ptrdiff_t>(headerSize));

	return datagram;
}


// The header at the start of a datagram; the body is whatever follows it. Returns nullopt for a header no link of this
// protocol version sends.
inline std::optional<DecodedPacket> decodeHeader(const std::span<const uint8_t> datagram)
{
	if (datagram.size() < BaseHeaderSize)
		return std::nullopt;

	const uint8_t *in = datagram.data();

	if (readUint16(in) != ProtocolMagic || in[2] != ProtocolVersion)
		return std::nullopt;

	DecodedPacket packet;
	PacketHeader &header = packet.header;
	header.flags		 = PacketFlags::fromRaw(in[3]);

	if (!header.flags.isValid() || header.flags.kind() == PacketKind::Beacon)
		return std::nullopt;

	header.srcStreamID = readUint32(in + 4);
	header.dstStreamID = readUint32(in + 8);
	header.seq		   = readUint64(in + 12);

	// A sender always has a stream ID, and its Data seqs start at 1
	if (header.srcStreamID == 0 || (header.flags.kind() == PacketKind::Data && header.seq == 0))
		return std::nullopt;

	size_t size = BaseHeaderSize;

	if (header.flags.isFragmented())
	{
		if (datagram.size() < size + FragmentExtensionSize)
			return std::nullopt;

		header.fragIndex = readUint16(in + size);
		header.fragCount = readUint16(in + size + 2);
		size += FragmentExtensionSize;

		// A message of one fragment is not fragmented, and no lane carries more than its largest message needs
		if (header.fragCount < 2 || header.fragCount > fragmentsOf(maxMessageSize(header.flags.lane())) || header.fragIndex >= header.fragCount)
			return std::nullopt;

		if (header.flags.isLastFragment() != (header.fragIndex + 1 == header.fragCount))
			return std::nullopt;
	}

	if (header.startsMessage())
	{
		if (datagram.size() < size + TagExtensionSize)
			return std::nullopt;

		header.tag = readUint32(in + size);
		size += TagExtensionSize;
	}

	if (header.startsFragmentedMessage())
	{
		if (datagram.size() < size + LengthExtensionSize)
			return std::nullopt;

		header.totalLength = readUint32(in + size);
		size += LengthExtensionSize;

		if (header.totalLength > maxMessageSize(header.flags.lane()) || fragmentsOf(header.totalLength) != header.fragCount)
			return std::nullopt;
	}

	packet.body = datagram.subspan(size);
	return packet;
}


// Whether a packet with that header carries a body of that size: nothing carries bytes it has no use for
inline bool isBodySizeValid(const PacketHeader &header, const size_t size)
{
	switch (header.flags.kind())
	{
	case PacketKind::Data:
		if (!header.flags.isFragmented())
			return size <= MaxFragmentBody;

		return header.flags.isLastFragment() ? size > 0 && size <= MaxFragmentBody : size == MaxFragmentBody;

	case PacketKind::Ping: return size == 0;

	case PacketKind::Ack:
	case PacketKind::Beacon: break; // the Ack body is checked where it is read (AckRanges.h)
	}

	return true;
}


// Returns nullopt for anything that is not a well-formed packet of a link in this protocol version
inline std::optional<DecodedPacket> decodePacket(const std::span<const uint8_t> datagram)
{
	if (datagram.size() > internal::MaxDatagramSize)
		return std::nullopt;

	auto packet = decodeHeader(datagram);

	if (!packet || !isBodySizeValid(packet->header, packet->body.size()))
		return std::nullopt;

	return packet;
}

} // namespace netlink::channel
