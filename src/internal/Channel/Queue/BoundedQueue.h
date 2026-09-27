/*
  ==============================================================================
	Module:         BoundedQueue
	Description:    Fixed-capacity FIFO ring buffer with a configurable policy for a full queue
  ==============================================================================
*/

#pragma once

#include <optional>
#include <utility>
#include <vector>

#include "NetLink/NetLink.h"


namespace netlink::channel
{

enum class PushResult
{
	Accepted,	   // Item queued, nothing dropped
	Rejected,	   // Queue full under DropNewest: the new item was refused
	EvictedOldest, // Queue full under DropOldest: the oldest item made room
};


template <typename T>
class BoundedQueue
{
public:
	explicit BoundedQueue(size_t capacity, const OverflowPolicy policy = OverflowPolicy::DropNewest) : mSlots(capacity > 0 ? capacity : 1), mPolicy(policy) {}

	// evicted receives the dropped item for PushResult::EvictedOldest
	PushResult push(T item, std::optional<T> *evicted = nullptr)
	{
		PushResult result = PushResult::Accepted;

		if (full())
		{
			if (mPolicy == OverflowPolicy::DropNewest)
				return PushResult::Rejected;

			auto oldest = pop();
			if (evicted)
				*evicted = std::move(oldest);

			result = PushResult::EvictedOldest;
		}

		mSlots[index(mSize)] = std::move(item);
		++mSize;
		return result;
	}

	std::optional<T> pop()
	{
		if (empty())
			return std::nullopt;

		std::optional<T> item(std::move(mSlots[mHead]));
		mSlots[mHead] = T{};
		mHead		  = index(1);
		--mSize;
		return item;
	}

	T		&front() { return mSlots[mHead]; }
	const T &front() const { return mSlots[mHead]; }

	// i = 0 is the oldest item
	T		&at(const size_t i) { return mSlots[index(i)]; }
	const T &at(const size_t i) const { return mSlots[index(i)]; }

	void	 clear()
	{
		while (!empty())
			pop();
		mHead = 0;
	}

	bool		   empty() const { return mSize == 0; }
	bool		   full() const { return mSize == mSlots.size(); }
	size_t		   size() const { return mSize; }
	size_t		   capacity() const { return mSlots.size(); }
	OverflowPolicy policy() const { return mPolicy; }

private:
	size_t		   index(const size_t offset) const { return (mHead + offset) % mSlots.size(); }

	std::vector<T> mSlots;
	size_t		   mHead{0};
	size_t		   mSize{0};
	OverflowPolicy mPolicy;
};

} // namespace netlink::channel
