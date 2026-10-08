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
// Under a sanitizer the runtime already replaces operator new, so there it is left alone.
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define NETLINK_SANITIZED
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define NETLINK_SANITIZED
#endif
#endif

#if (!defined(_WIN32) || defined(NETLINK_TEST_COUNT_ALLOCATIONS)) && !defined(NETLINK_SANITIZED)
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
