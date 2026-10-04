#include <gtest/gtest.h>

#include <initializer_list>
#include <memory>

#include "AllocationCounter.h"
#include "Channel/Reliability/ReliableLink.h"
#include "QueueSource.h"

using namespace netlink::channel;


namespace ChannelTests
{

// What one of two links holds on the heap after they exchanged a message on each of these lanes and are idle again
static std::int64_t bytesPerLinkAfter(const std::initializer_list<Lane> lanes)
{
	FakeNet::QueueSource source;
	FakeNet::QueueSource none;
	const auto			 now = ReliableLink::Clock::now();

	FakeNet::resetLiveBytes();
	FakeNet::countAllocations(true);

	auto link  = std::make_unique<ReliableLink>();
	auto other = std::make_unique<ReliableLink>();

	FakeNet::countAllocations(false);

	for (const Lane lane : lanes)
		source.push(lane, 0, std::vector<uint8_t>(200));

	const auto carry = [&](ReliableLink &from, ReliableLink &to, FakeNet::QueueSource &messages)
	{
		for (const auto &datagram : FakeNet::takeOutgoing(from, now, messages))
		{
			const auto bytes = FakeNet::bytesOf(datagram);
			to.onPacket(*decodePacket(bytes), now);
		}
	};

	FakeNet::countAllocations(true);
	carry(*link, *other, source);
	carry(*other, *link, none);
	link->takeDelivered();
	other->takeDelivered();
	FakeNet::countAllocations(false);

	// Both links hold the same: the lanes that sent or received one message
	const std::int64_t perLink = FakeNet::liveBytes() / 2;

	link.reset();
	other.reset();

	EXPECT_EQ(FakeNet::liveBytes(), 0) << "Everything a link took is given back with it";
	return perLink;
}


TEST(LinkMemory, ControlOnlyLink_StaysSmall)
{
	if (!FakeNet::countsAllocations())
		GTEST_SKIP() << "Allocations cannot be counted in this build";

	const auto perLink = bytesPerLinkAfter({Lane::Control});

	EXPECT_GT(perLink, 0);
	EXPECT_LE(perLink, 16 * 1024) << "Most peers are only ever discovered and greeted: that has to stay cheap";
}


TEST(LinkMemory, LinkWithControlAndReliable_StaysSmall)
{
	if (!FakeNet::countsAllocations())
		GTEST_SKIP() << "Allocations cannot be counted in this build";

	// What every session uses: its Control lane and the default lane for messages
	const auto perLink = bytesPerLinkAfter({Lane::Control, Lane::Reliable});

	EXPECT_GT(perLink, 16 * 1024);
	EXPECT_LE(perLink, 64 * 1024) << "A session costs this much for as long as it is idle";
}

} // namespace ChannelTests
