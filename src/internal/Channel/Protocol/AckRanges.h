/*
  ==============================================================================
	Module:         AckRanges
	Description:    Body of an Ack: what the receiver of a lane holds, besides
					everything up to the seq in the header
  ==============================================================================
*/

#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ByteOrder.h"


/*
 Ack body:
	0   u32   serial          counts the Acks a link sends: tells a late Ack from the latest one
	4   u32   mediaReceived   Media datagrams this link received so far
	8   ranges                seqs that wait behind a gap, ascending

 Range:
	0   u64   first     first seq of the range
	8   u16   count     number of consecutive seqs, at least 1
 */


namespace netlink::channel
{

inline constexpr size_t AckFieldsSize = 8;
inline constexpr size_t SeqRangeSize  = 10;


struct SeqRange
{
	uint64_t first{0};
	uint16_t count{0};

	uint64_t last() const { return first + count - 1; }
	bool	 operator==(const SeqRange &other) const = default;
};


struct AckBody
{
	uint32_t			  serial{0};
	uint32_t			  mediaReceived{0};
	std::vector<SeqRange> ranges;
};


// At most maxRanges of them: the lowest seqs, which the sender needs first
inline std::vector<uint8_t> encodeAck(const AckBody &ack, const size_t maxRanges)
{
	const size_t		 count = std::min(ack.ranges.size(), maxRanges);
	std::vector<uint8_t> body(AckFieldsSize + count * SeqRangeSize);

	writeUint32(body.data(), ack.serial);
	writeUint32(body.data() + 4, ack.mediaReceived);

	for (size_t i = 0; i < count; ++i)
	{
		uint8_t *out = body.data() + AckFieldsSize + i * SeqRangeSize;
		writeUint64(out, ack.ranges[i].first);
		writeUint16(out + 8, ack.ranges[i].count);
	}

	return body;
}


// Returns nullopt for a body that is not the two fields and a whole number of well-formed ranges
inline std::optional<AckBody> decodeAck(const std::span<const uint8_t> body)
{
	if (body.size() < AckFieldsSize || (body.size() - AckFieldsSize) % SeqRangeSize != 0)
		return std::nullopt;

	AckBody ack;
	ack.serial		  = readUint32(body.data());
	ack.mediaReceived = readUint32(body.data() + 4);
	ack.ranges.reserve((body.size() - AckFieldsSize) / SeqRangeSize);

	for (size_t offset = AckFieldsSize; offset < body.size(); offset += SeqRangeSize)
	{
		const SeqRange range{.first = readUint64(body.data() + offset), .count = readUint16(body.data() + offset + 8)};

		// Empty ranges are never sent, and seqs start at 1 and do not wrap
		if (range.count == 0 || range.first == 0 || range.last() < range.first)
			return std::nullopt;

		ack.ranges.push_back(range);
	}

	return ack;
}


// The seqs as as few ranges as possible, ascending. Sorts the seqs and drops duplicates.
inline std::vector<SeqRange> toRanges(std::vector<uint64_t> &seqs)
{
	std::vector<SeqRange> ranges;

	if (seqs.empty())
		return ranges;

	if (!std::ranges::is_sorted(seqs))
		std::ranges::sort(seqs);

	for (const uint64_t seq : seqs)
	{
		if (!ranges.empty() && seq <= ranges.back().last())
			continue; // duplicate

		if (!ranges.empty() && seq == ranges.back().last() + 1 && ranges.back().count < UINT16_MAX)
			++ranges.back().count;
		else
			ranges.push_back({.first = seq, .count = 1});
	}

	return ranges;
}

} // namespace netlink::channel
