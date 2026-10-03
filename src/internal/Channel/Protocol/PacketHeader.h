/*
  ==============================================================================
	Module:         PacketHeader
	Description:    Header of every datagram on the peer channel and its binary encoding (network byte order)
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


namespace netlink::channel
{

inline constexpr uint16_t ProtocolMagic			= 0x4E4C;
inline constexpr uint8_t  ProtocolVersion		= 2;
inline constexpr size_t	  BaseHeaderSize		= 20;
inline constexpr size_t	  FragmentExtensionSize = 4;
inline constexpr size_t	  TagExtensionSize		= 4;
inline constexpr size_t	  MaxHeaderSize			= BaseHeaderSize + FragmentExtensionSize + TagExtensionSize;


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

 seq per packet kind (reliable packets count per channel, each channel is a stream of its own):
	Data		seq of this packet
	DataAck		highest seq received without a gap; the body lists further seqs (see AckRanges.h)
	AckAck		every DataAck up to this seq arrived; the body lists further seqs
	Heartbeat	unused (0)
 */
struct PacketHeader
{
	PacketFlags flags{};
	uint32_t	srcStreamID{0}; // random ID of the sender's current stream; changes when that stream restarts
	uint32_t	dstStreamID{0}; // 0 while the sender does not know the receiver yet
	uint64_t	seq{0};
	uint16_t	fragIndex{0}; // only on the wire when flags.isFragmented()
	uint16_t	fragCount{0};
	uint32_t	tag{0}; // only on the wire when startsMessage()

	// The packet carries the beginning of a message, and with it the message's tag
	bool		startsMessage() const { return flags.kind() == PacketKind::Data && (!flags.isFragmented() || fragIndex == 0); }

	size_t		encodedSize() const { return BaseHeaderSize + (flags.isFragmented() ? FragmentExtensionSize : 0) + (startsMessage() ? TagExtensionSize : 0); }
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


// Returns nullopt for anything that is not a well-formed packet of this protocol version
inline std::optional<DecodedPacket> decodePacket(const std::span<const uint8_t> datagram)
{
	if (datagram.size() < BaseHeaderSize)
		return std::nullopt;

	const uint8_t *in = datagram.data();

	if (readUint16(in) != ProtocolMagic || in[2] != ProtocolVersion)
		return std::nullopt;

	DecodedPacket packet;
	packet.header.flags = PacketFlags::fromRaw(in[3]);

	if (!packet.header.flags.isValid())
		return std::nullopt;

	packet.header.srcStreamID = readUint32(in + 4);
	packet.header.dstStreamID = readUint32(in + 8);
	packet.header.seq		  = readUint64(in + 12);

	// A sender always has a stream ID
	if (packet.header.srcStreamID == 0)
		return std::nullopt;

	size_t size = BaseHeaderSize;

	if (packet.header.flags.isFragmented())
	{
		if (datagram.size() < size + FragmentExtensionSize)
			return std::nullopt;

		packet.header.fragIndex = readUint16(in + size);
		packet.header.fragCount = readUint16(in + size + 2);
		size += FragmentExtensionSize;

		if (packet.header.fragCount == 0 || packet.header.fragIndex >= packet.header.fragCount)
			return std::nullopt;

		// The last-fragment bit must agree with the index
		if (packet.header.flags.isLastFragment() != (packet.header.fragIndex + 1 == packet.header.fragCount))
			return std::nullopt;
	}

	if (packet.header.startsMessage())
	{
		if (datagram.size() < size + TagExtensionSize)
			return std::nullopt;

		packet.header.tag = readUint32(in + size);
		size += TagExtensionSize;
	}

	packet.body = datagram.subspan(size);
	return packet;
}

} // namespace netlink::channel
