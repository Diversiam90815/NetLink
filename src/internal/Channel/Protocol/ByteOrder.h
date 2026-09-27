/*
  ==============================================================================
	Module:         ByteOrder
	Description:    Big-endian read/write helpers for the channel wire format
  ==============================================================================
*/

#pragma once

namespace netlink::channel
{

inline void writeUint16(uint8_t *out, const uint16_t value)
{
	out[0] = static_cast<uint8_t>(value >> 8);
	out[1] = static_cast<uint8_t>(value);
}


inline void writeUint32(uint8_t *out, const uint32_t value)
{
	out[0] = static_cast<uint8_t>(value >> 24);
	out[1] = static_cast<uint8_t>(value >> 16);
	out[2] = static_cast<uint8_t>(value >> 8);
	out[3] = static_cast<uint8_t>(value);
}


inline void writeUint64(uint8_t *out, const uint64_t value)
{
	writeUint32(out, static_cast<uint32_t>(value >> 32));
	writeUint32(out + 4, static_cast<uint32_t>(value));
}


inline uint16_t readUint16(const uint8_t *in)
{
	return static_cast<uint16_t>((static_cast<uint16_t>(in[0]) << 8) | static_cast<uint16_t>(in[1]));
}


inline uint32_t readUint32(const uint8_t *in)
{
	return (static_cast<uint32_t>(in[0]) << 24) | (static_cast<uint32_t>(in[1]) << 16) | (static_cast<uint32_t>(in[2]) << 8) | static_cast<uint32_t>(in[3]);
}


inline uint64_t readUint64(const uint8_t *in)
{
	return (static_cast<uint64_t>(readUint32(in)) << 32) | static_cast<uint64_t>(readUint32(in + 4));
}

} // namespace netlink::channel
