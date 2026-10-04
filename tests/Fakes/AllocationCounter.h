/*
  ==============================================================================
	Module:         AllocationCounter
	Description:    Counts the bytes the calling thread holds on the heap, for
					tests that bound what something costs in memory
  ==============================================================================
*/

#pragma once

#include <cstdint>


// Replacing operator new only reaches every allocation where the whole program shares one definition of it. On Windows
// the standard library in its DLL keeps its own, and memory would be taken with one and given back with the other.
#if !defined(_WIN32) || defined(NETLINK_TEST_COUNT_ALLOCATIONS)
#define NETLINK_COUNTS_ALLOCATIONS
#endif


namespace FakeNet
{

// False where allocations cannot be counted: a test of memory skips itself then
bool		 countsAllocations();

// What the calling thread allocates from now on is counted (or not anymore). Giving it back always is.
void		 countAllocations(bool counting);

std::int64_t liveBytes();
void		 resetLiveBytes();

} // namespace FakeNet
