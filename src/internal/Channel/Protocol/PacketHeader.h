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
inline constexpr uint8_t  ProtocolVersion		= 1;
inline constexpr size_t	  BaseHeaderSize		= 20;
inline constexpr size_t	  FragmentExtensionSize = 4;


// Uniquely identifies a reliable message (fragment) sent by one incarnation of a peer
struct MessageKey
{
	uint32_t incarnation{0};
	uint64_t seq{0};

	bool	 operator==(const MessageKey &other) const = default;
};


/*
 Packet head decoding:

					0	u16		magic 0x4E4C ("NL")
					2	u8		version
					3	u8		flags
					4	u32		srcIncarnation
					8	u32		dstIncarnation (0 = unknown)
					12	u64		seq (message key) (20 bytes)
[flags.Fragmented]	20	u16		fragIndex
					22	u16		fragCount (24 bytes)
 */
struct PacketHeader
{
	PacketFlags flags{};
	uint32_t	srcIncarnation{0};
	uint32_t	dstIncarnation{0}; // 0 while the sender does not know the receiver yet
	uint64_t	seq{0};			   // acknowledged seq for DataAck / AckAck
	uint16_t	fragIndex{0};	   // only on the wire when flags.isFragmented()
	uint16_t	fragCount{0};

	size_t		encodedSize() const { return BaseHeaderSize + (flags.isFragmented() ? FragmentExtensionSize : 0); }
	MessageKey	key() const { return {srcIncarnation, seq}; }
};


struct DecodedPacket
{
	PacketHeader			 header;
	std::span<const uint8_t> body; // points into the decoded datagram
};


inline std::vector<uint8_t> encodePacket(const PacketHeader &header, std::span<const uint8_t> body = {})
{
	std::vector<uint8_t> datagram(header.encodedSize() + body.size());
	uint8_t				*out = datagram.data();

	writeUint16(out, ProtocolMagic);
	out[2] = ProtocolVersion;
	out[3] = header.flags.raw();
	writeUint32(out + 4, header.srcIncarnation);
	writeUint32(out + 8, header.dstIncarnation);
	writeUint64(out + 12, header.seq);

	if (header.flags.isFragmented())
	{
		writeUint16(out + BaseHeaderSize, header.fragIndex);
		writeUint16(out + BaseHeaderSize + 2, header.fragCount);
	}

	if (!body.empty())
		std::copy(body.begin(), body.end(), datagram.begin() + static_cast<std::ptrdiff_t>(header.encodedSize()));

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

	packet.header.srcIncarnation = readUint32(in + 4);
	packet.header.dstIncarnation = readUint32(in + 8);
	packet.header.seq			 = readUint64(in + 12);

	// A sender always has an incarnation
	if (packet.header.srcIncarnation == 0)
		return std::nullopt;

	if (packet.header.flags.isFragmented())
	{
		if (datagram.size() < BaseHeaderSize + FragmentExtensionSize)
			return std::nullopt;

		packet.header.fragIndex = readUint16(in + BaseHeaderSize);
		packet.header.fragCount = readUint16(in + BaseHeaderSize + 2);

		if (packet.header.fragCount == 0 || packet.header.fragIndex >= packet.header.fragCount)
			return std::nullopt;

		// The last-fragment bit must agree with the index
		if (packet.header.flags.isLastFragment() != (packet.header.fragIndex + 1 == packet.header.fragCount))
			return std::nullopt;
	}

	packet.body = datagram.subspan(packet.header.encodedSize());
	return packet;
}

} // namespace netlink::channel
