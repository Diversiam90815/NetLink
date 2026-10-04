/*
  ==============================================================================
	Module:         SequenceBuffer
	Description:    Ring buffer keyed by a 64-bit sequence number
  ==============================================================================
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>


namespace netlink::channel
{

template <typename T>
class SequenceBuffer
{
public:
	// The capacity must be a power of two. Seqs that are a capacity apart share a slot: the newer one replaces the older.
	explicit SequenceBuffer(const size_t capacity) : mSlots(capacity) {}

	size_t capacity() const { return mSlots.size(); }

	// Stores the entry, replacing whatever occupied its slot (same seq or an older one)
	T	  &insert(uint64_t seq, T value)
	{
		Slot &slot = mSlots[index(seq)];

		if (!slot.occupied)
			++mSize;

		slot.seq	  = seq;
		slot.occupied = true;
		slot.value	  = std::move(value);
		return slot.value;
	}

	T *find(uint64_t seq)
	{
		Slot &slot = mSlots[index(seq)];
		return slot.occupied && slot.seq == seq ? &slot.value : nullptr;
	}

	const T *find(uint64_t seq) const
	{
		const Slot &slot = mSlots[index(seq)];
		return slot.occupied && slot.seq == seq ? &slot.value : nullptr;
	}

	bool			 contains(const uint64_t seq) const { return find(seq) != nullptr; }

	// Removes and returns the entry for seq, if present
	std::optional<T> take(uint64_t seq)
	{
		Slot &slot = mSlots[index(seq)];

		if (!slot.occupied || slot.seq != seq)
			return std::nullopt;

		std::optional<T> value(std::move(slot.value));
		release(slot);
		return value;
	}

	bool erase(uint64_t seq)
	{
		Slot &slot = mSlots[index(seq)];

		if (!slot.occupied || slot.seq != seq)
			return false;

		release(slot);
		return true;
	}

	void clear()
	{
		for (auto &slot : mSlots)
		{
			if (slot.occupied)
				release(slot);
		}
	}

	size_t size() const { return mSize; }
	bool   empty() const { return mSize == 0; }

	// Visits every occupied entry as fn(seq, T&). fn returns false to erase the entry.
	template <typename Fn>
	void forEach(Fn &&fn)
	{
		for (auto &slot : mSlots)
		{
			if (slot.occupied && !fn(slot.seq, slot.value))
				release(slot);
		}
	}

private:
	struct Slot
	{
		uint64_t seq{0};
		bool	 occupied{false};
		T		 value{};
	};

	size_t index(const uint64_t seq) const { return static_cast<size_t>(seq & (mSlots.size() - 1)); }

	void   release(Slot &slot)
	{
		slot.occupied = false;
		slot.value	  = T{};
		--mSize;
	}

	std::vector<Slot> mSlots;
	size_t			  mSize{0};
};

} // namespace netlink::channel
