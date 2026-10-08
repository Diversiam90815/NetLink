/*
  ==============================================================================
	Module:         MediaAssembler
	Description:    Puts the fragments of unacknowledged messages together:
					they arrive in any order, and some never arrive
  ==============================================================================
*/

#include "MediaAssembler.h"

#include <algorithm>
#include <cstring>


namespace netlink::channel
{

std::optional<AssembledMessage> MediaAssembler::accept(const PacketHeader &header, const std::span<const uint8_t> body)
{
	if (!header.flags.isFragmented())
		return AssembledMessage{.tag = header.tag, .body = std::vector<uint8_t>(body.begin(), body.end())};

	// One bit per fragment, and a seq that cannot belong to a message
	if (header.fragCount > 64 || header.seq <= header.fragIndex)
		return std::nullopt;

	const bool last = header.flags.isLastFragment();
	if (last ? body.empty() || body.size() > MaxFragmentBody : body.size() != MaxFragmentBody)
		return std::nullopt;

	Slot *slot = slotFor(header.seq - header.fragIndex, header.fragCount);
	if (!slot)
		return std::nullopt;

	const uint64_t bit = uint64_t{1} << header.fragIndex;

	if (slot->count != header.fragCount || (slot->present & bit) != 0)
		return std::nullopt;

	std::memcpy(slot->body.data() + static_cast<size_t>(header.fragIndex) * MaxFragmentBody, body.data(), body.size());
	slot->present |= bit;
	++slot->received;

	if (header.fragIndex == 0)
	{
		slot->tag		  = header.tag;
		slot->totalLength = header.totalLength;
	}

	if (last)
		slot->lastLength = static_cast<uint32_t>(body.size());

	if (slot->received < slot->count)
		return std::nullopt;

	const size_t	 length	  = static_cast<size_t>(slot->count - 1) * MaxFragmentBody + slot->lastLength;
	const bool		 complete = length == slot->totalLength;

	AssembledMessage message{.tag = slot->tag, .body = std::move(slot->body)};
	*slot = {};

	if (!complete)
		return std::nullopt;

	message.body.resize(length);
	return message;
}


size_t MediaAssembler::inProgress() const
{
	return static_cast<size_t>(std::ranges::count_if(mSlots, [](const Slot &slot) { return slot.used; }));
}


MediaAssembler::Slot *MediaAssembler::slotFor(const uint64_t id, const uint16_t count)
{
	Slot *free	 = nullptr;
	Slot *oldest = nullptr;

	for (Slot &slot : mSlots)
	{
		if (slot.used && slot.id == id)
			return &slot;

		if (!slot.used)
			free = &slot;
		else if (!oldest || slot.id < oldest->id)
			oldest = &slot;
	}

	if (!free)
	{
		// A straggler of a message older than everything in progress is not worth giving up a newer one for
		if (!oldest || id < oldest->id)
			return nullptr;

		*oldest = {};
		free	= oldest;
		++mAbandoned;
	}

	free->used	= true;
	free->id	= id;
	free->count = count;
	free->body.resize(static_cast<size_t>(count) * MaxFragmentBody);
	return free;
}

} // namespace netlink::channel
