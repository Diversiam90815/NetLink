/*
  ==============================================================================
	Module:         AckRanges
	Description:    Body of DataAck and AckAck packets: the acknowledged seqs,
					as ranges, so one datagram acknowledges many packets
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
 DataAck body:
	0   u16   window    packets the receiver accepts in flight on this channel (0 = pause, its application is not keeping up)
	2   ranges

 AckAck body:
	0   ranges

 Range:
	0   u64   first     first seq of the range
	8   u16   count     number of consecutive seqs, at least 1
 */


namespace netlink::channel
{

inline constexpr size_t AckWindowFieldSize = 2;
inline constexpr size_t SeqRangeSize	   = 10;


struct SeqRange
{
	uint64_t first{0};
	uint16_t count{0};

	uint64_t last() const { return first + count - 1; }
	bool	 operator==(const SeqRange &other) const = default;
};


inline void appendRange(std::vector<uint8_t> &body, const SeqRange &range)
{
	const size_t offset = body.size();
	body.resize(offset + SeqRangeSize);
	writeUint64(body.data() + offset, range.first);
	writeUint16(body.data() + offset + 8, range.count);
}


// Returns nullopt for a body that is not a whole number of well-formed ranges
inline std::optional<std::vector<SeqRange>> decodeRanges(const std::span<const uint8_t> body)
{
	if (body.size() % SeqRangeSize != 0)
		return std::nullopt;

	std::vector<SeqRange> ranges;
	ranges.reserve(body.size() / SeqRangeSize);

	for (size_t offset = 0; offset < body.size(); offset += SeqRangeSize)
	{
		const SeqRange range{.first = readUint64(body.data() + offset), .count = readUint16(body.data() + offset + 8)};

		// Empty ranges are never sent, and seqs start at 1 and do not wrap
		if (range.count == 0 || range.first == 0 || range.last() < range.first)
			return std::nullopt;

		ranges.push_back(range);
	}

	return ranges;
}


// The seqs as as few ranges as possible, ascending. Sorts the seqs and drops duplicates.
inline std::vector<SeqRange> toRanges(std::vector<uint64_t> &seqs)
{
	std::vector<SeqRange> ranges;

	if (seqs.empty())
		return ranges;

	// Packets mostly arrive in order: nothing to do then
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
