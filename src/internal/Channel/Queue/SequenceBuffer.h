/*
  ==============================================================================
	Module:         SequenceBuffer
	Description:    Fixed-size store for entries keyed by a 64-bit sequence
					number, indexed directly by seq % Capacity
  ==============================================================================
*/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>


namespace netlink::channel
{

template <typename T, size_t Capacity>
class SequenceBuffer
{
	static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
	static constexpr size_t capacity() { return Capacity; }

	// Stores the entry, replacing whatever occupied its slot (same seq or an older one)
	T					   &insert(uint64_t seq, T value)
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

	static constexpr size_t index(const uint64_t seq) { return static_cast<size_t>(seq & (Capacity - 1)); }

	void					release(Slot &slot)
	{
		slot.occupied = false;
		slot.value	  = T{};
		--mSize;
	}

	std::array<Slot, Capacity> mSlots{};
	size_t					   mSize{0};
};

} // namespace netlink::channel
