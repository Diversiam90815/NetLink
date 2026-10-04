#include <gtest/gtest.h>

#include <cstdlib>
#include <new>

#include "Channel/Reliability/ReliableLink.h"
#include "QueueSource.h"

using namespace netlink::channel;


// Replacing operator new only reaches every allocation where the whole program shares one definition of it. On Windows
// the standard library in its DLL keeps its own, and memory would be taken with one and given back with the other.
#if !defined(_WIN32) || defined(NETLINK_TEST_COUNT_ALLOCATIONS)
#define NETLINK_COUNTS_ALLOCATIONS
#endif

#ifdef NETLINK_COUNTS_ALLOCATIONS

// Counts the bytes the calling thread holds on the heap, while it is asked to. The size is kept in front of each block,
// so a block can be given back without knowing how large it was.
namespace
{

constexpr std::size_t	  HeaderSize = 16; // keeps the alignment of what malloc returns

thread_local bool		  counting	 = false;
thread_local std::int64_t liveBytes	 = 0;

void					 *allocate(const std::size_t size)
{
	void *block = std::malloc(size + HeaderSize);
	if (!block)
		throw std::bad_alloc();

	*static_cast<std::size_t *>(block) = counting ? size : 0;
	liveBytes += counting ? static_cast<std::int64_t>(size) : 0;
	return static_cast<char *>(block) + HeaderSize;
}

void release(void *pointer) noexcept
{
	if (!pointer)
		return;

	void *block = static_cast<char *>(pointer) - HeaderSize;
	liveBytes -= static_cast<std::int64_t>(*static_cast<std::size_t *>(block));
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

#endif


namespace ChannelTests
{

TEST(LinkMemory, ControlOnlyLink_StaysSmall)
{
#ifndef NETLINK_COUNTS_ALLOCATIONS
	GTEST_SKIP() << "Allocations cannot be counted in this build";
#else
	FakeNet::QueueSource source;
	FakeNet::QueueSource none;
	const auto			 now = ReliableLink::Clock::now();

	liveBytes				 = 0;
	counting				 = true;

	// A link that exchanged control signals in both directions and is idle again
	auto link				 = std::make_unique<ReliableLink>();
	auto other				 = std::make_unique<ReliableLink>();

	counting				 = false;

	source.push(Lane::Control, 0, std::vector<uint8_t>(200));

	const auto carry = [&](ReliableLink &from, ReliableLink &to, FakeNet::QueueSource &messages)
	{
		for (const auto &datagram : from.takeOutgoing(now, messages))
		{
			const auto bytes = datagram.bytes();
			to.onPacket(*decodePacket(bytes), now);
		}
	};

	counting = true;
	carry(*link, *other, source);
	carry(*other, *link, none);
	link->takeDelivered();
	other->takeDelivered();
	counting				   = false;

	// Both links hold the same: one Control lane that sent or received one message
	const std::int64_t perLink = liveBytes / 2;

	EXPECT_GT(perLink, 0);
	EXPECT_LE(perLink, 16 * 1024) << "Most peers are only ever discovered and greeted: that has to stay cheap";

	counting = true;
	link.reset();
	other.reset();
	counting = false;

	EXPECT_EQ(liveBytes, 0) << "Everything a link took is given back with it";
#endif
}


TEST(LinkMemory, LinkWithControlAndReliable_StaysSmall)
{
#ifndef NETLINK_COUNTS_ALLOCATIONS
	GTEST_SKIP() << "Allocations cannot be counted in this build";
#else
	FakeNet::QueueSource source;
	FakeNet::QueueSource none;
	const auto			 now = ReliableLink::Clock::now();

	liveBytes				 = 0;
	counting				 = true;

	auto link				 = std::make_unique<ReliableLink>();
	auto other				 = std::make_unique<ReliableLink>();

	counting				 = false;

	// What every session uses: its Control lane and the default lane for messages, in both directions
	for (const Lane lane : {Lane::Control, Lane::Reliable})
		source.push(lane, 0, std::vector<uint8_t>(200));

	const auto carry = [&](ReliableLink &from, ReliableLink &to, FakeNet::QueueSource &messages)
	{
		for (const auto &datagram : from.takeOutgoing(now, messages))
		{
			const auto bytes = datagram.bytes();
			to.onPacket(*decodePacket(bytes), now);
		}
	};

	counting = true;
	carry(*link, *other, source);
	carry(*other, *link, none);
	link->takeDelivered();
	other->takeDelivered();
	counting				   = false;

	const std::int64_t perLink = liveBytes / 2;

	EXPECT_GT(perLink, 16 * 1024);
	EXPECT_LE(perLink, 64 * 1024) << "A session costs this much for as long as it is idle";

	link.reset();
	other.reset();
#endif
}

} // namespace ChannelTests
