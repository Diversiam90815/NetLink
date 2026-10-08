/*
  ==============================================================================
	Module:         AllocationCounter
	Description:    Counts the bytes the calling thread holds on the heap, for
					tests that bound what something costs in memory
  ==============================================================================
*/

#include "AllocationCounter.h"

#include <cstdlib>
#include <new>


#ifdef NETLINK_COUNTS_ALLOCATIONS

// The size is kept in front of each block, so a block can be given back without knowing how large it was
namespace
{

constexpr std::size_t	  HeaderSize = 16; // keeps the alignment of what malloc returns

thread_local bool		  counting	 = false;
thread_local std::int64_t live		 = 0;

void					 *allocate(const std::size_t size)
{
	void *block = std::malloc(size + HeaderSize);
	if (!block)
		throw std::bad_alloc();

	*static_cast<std::size_t *>(block) = counting ? size : 0;
	live += counting ? static_cast<std::int64_t>(size) : 0;
	return static_cast<char *>(block) + HeaderSize;
}

void release(void *pointer) noexcept
{
	if (!pointer)
		return;

	void *block = static_cast<char *>(pointer) - HeaderSize;
	live -= static_cast<std::int64_t>(*static_cast<std::size_t *>(block));
	std::free(block);
}

} // namespace

void *operator new(const std::size_t size)
{
	return allocate(size);
}
void *operator new[](const std::size_t size)
{
	return allocate(size);
}
void operator delete(void *pointer) noexcept
{
	release(pointer);
}
void operator delete[](void *pointer) noexcept
{
	release(pointer);
}
void operator delete(void *pointer, std::size_t) noexcept
{
	release(pointer);
}
void operator delete[](void *pointer, std::size_t) noexcept
{
	release(pointer);
}

bool FakeNet::countsAllocations()
{
	return true;
}

void FakeNet::countAllocations(const bool on)
{
	counting = on;
}

std::int64_t FakeNet::liveBytes()
{
	return live;
}

void FakeNet::resetLiveBytes()
{
	live = 0;
}

#else

bool FakeNet::countsAllocations()
{
	return false;
}

void		 FakeNet::countAllocations(bool) {}

std::int64_t FakeNet::liveBytes()
{
	return 0;
}

void FakeNet::resetLiveBytes() {}

#endif
