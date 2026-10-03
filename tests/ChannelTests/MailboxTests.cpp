#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <utility>
#include <vector>

#include "Channel/Mailbox.h"
#include "TestIp.h"

using namespace netlink;
using namespace netlink::channel;
using namespace std::chrono_literals;


namespace ChannelTests
{

class MailboxTest : public ::testing::Test
{
protected:
	using Lane	  = Mailbox::Lane;
	using Push	  = Mailbox::Push;
	using Command = Mailbox::Command;
	using Taken	  = std::vector<std::pair<Lane, uint32_t>>;

	void SetUp() override
	{
		mailbox.setLimits(limits);
		mailbox.open(peer);
		mailbox.setRunning(true);
	}

	static Mailbox::Mail mail(const uint32_t tag, const size_t size = 1) { return {.tag = tag, .body = std::vector<uint8_t>(size)}; }

	// Takes what the mailbox hands over for the peer, like a link with room for `room` messages
	Taken				 take(const size_t room = SIZE_MAX)
	{
		Taken taken;

		stillWaiting = mailbox.feed(peer,
									[&](const Lane lane, const Mailbox::Mail &message)
									{
										if (taken.size() >= room)
											return false;

										taken.emplace_back(lane, message.tag);
										return true;
									});
		return taken;
	}

	void fillApplicationLane()
	{
		for (uint32_t i = 0; i < limits.applicationCapacity; ++i)
			ASSERT_EQ(mailbox.push(peer, Lane::Application, mail(i)), Push::Queued);
	}

	Mailbox::Limits limits{
		.controlCapacity = 4, .applicationCapacity = 3, .applicationOverflow = OverflowPolicy::DropNewest, .unreliableCapacity = 2, .maxMessageSize = 100, .maxUnreliableBody = 10};

	std::atomic<int>   rings{0};
	Mailbox			   mailbox{[this] { ++rings; }};
	net::SocketAddress peer{ipv4("10.0.0.2"), 4000};
	Mailbox::Work	   work;
	bool			   stillWaiting{false};
};


TEST_F(MailboxTest, Messages_AreHandedOverInOrderPerLane)
{
	EXPECT_EQ(mailbox.push(peer, Lane::Application, mail(1)), Push::Queued);
	EXPECT_EQ(mailbox.push(peer, Lane::Unreliable, mail(2)), Push::Queued);
	EXPECT_EQ(mailbox.push(peer, Lane::Application, mail(3)), Push::Queued);
	EXPECT_EQ(mailbox.push(peer, Lane::Control, mail(4)), Push::Queued);

	EXPECT_EQ(take(), (Taken{{Lane::Control, 4}, {Lane::Application, 1}, {Lane::Application, 3}, {Lane::Unreliable, 2}})) << "Control first, every lane in the order it was filled";
	EXPECT_FALSE(stillWaiting);
	EXPECT_TRUE(take().empty()) << "Handed over once";
}


TEST_F(MailboxTest, LinkWithoutRoom_LeavesTheMessagesWaiting)
{
	fillApplicationLane();

	EXPECT_EQ(take(2), (Taken{{Lane::Application, 0}, {Lane::Application, 1}}));
	EXPECT_TRUE(stillWaiting);

	EXPECT_EQ(take(), (Taken{{Lane::Application, 2}}));
	EXPECT_FALSE(stillWaiting);
}


TEST_F(MailboxTest, FullLane_RefusesTheNewestMessage)
{
	fillApplicationLane();

	EXPECT_EQ(mailbox.push(peer, Lane::Application, mail(9)), Push::Full);
	EXPECT_EQ(mailbox.push(peer, Lane::Control, mail(9)), Push::Queued) << "Every lane has room of its own";
}


TEST_F(MailboxTest, DropOldest_MakesRoomForTheNewestMessage)
{
	limits.applicationOverflow = OverflowPolicy::DropOldest;
	mailbox.setLimits(limits);

	for (uint32_t i = 1; i <= 5; ++i)
		EXPECT_EQ(mailbox.push(peer, Lane::Application, mail(i)), Push::Queued);

	for (uint32_t i = 1; i <= 3; ++i)
		EXPECT_EQ(mailbox.push(peer, Lane::Unreliable, mail(i)), Push::Queued) << "Unreliable messages never wait for room";

	EXPECT_EQ(take(), (Taken{{Lane::Application, 3}, {Lane::Application, 4}, {Lane::Application, 5}, {Lane::Unreliable, 2}, {Lane::Unreliable, 3}}));
}


TEST_F(MailboxTest, OversizedMessages_AreRefused)
{
	EXPECT_EQ(mailbox.push(peer, Lane::Application, mail(1, 100)), Push::Queued);
	EXPECT_EQ(mailbox.push(peer, Lane::Application, mail(1, 101)), Push::TooLarge);
	EXPECT_EQ(mailbox.push(peer, Lane::Unreliable, mail(1, 10)), Push::Queued);
	EXPECT_EQ(mailbox.push(peer, Lane::Unreliable, mail(1, 11)), Push::TooLarge) << "An unreliable message has to fit into one datagram";
}


TEST_F(MailboxTest, UnknownPeer_IsClosed)
{
	const net::SocketAddress stranger{ipv4("10.0.0.9"), 1};

	EXPECT_EQ(mailbox.push(stranger, Lane::Application, mail(1)), Push::Closed);
	EXPECT_TRUE(mailbox.flush(stranger, 1s)) << "Nothing is pending for a peer that is not there";
}


TEST_F(MailboxTest, Doorbell_RingsWhenWorkArrivesAndNoneWasWaiting)
{
	mailbox.push(peer, Lane::Application, mail(1));
	mailbox.push(peer, Lane::Application, mail(2));
	mailbox.post(Command::KeepAliveOn, peer);
	EXPECT_EQ(rings.load(), 1) << "The I/O thread was told already";

	mailbox.drain(work);
	EXPECT_EQ(rings.load(), 1);

	mailbox.post(Command::KeepAliveOff, peer);
	EXPECT_EQ(rings.load(), 2) << "After a drain the next work rings again";
}


TEST_F(MailboxTest, Drain_ListsAPeerWithNewMessagesOnce)
{
	mailbox.push(peer, Lane::Application, mail(1));
	mailbox.push(peer, Lane::Control, mail(2));

	mailbox.drain(work);
	EXPECT_EQ(work.ready, (std::vector<net::SocketAddress>{peer}));
	EXPECT_TRUE(work.commands.empty());

	mailbox.drain(work);
	EXPECT_TRUE(work.ready.empty());

	mailbox.push(peer, Lane::Application, mail(3));
	mailbox.drain(work);
	EXPECT_EQ(work.ready, (std::vector<net::SocketAddress>{peer}));
}


TEST_F(MailboxTest, Commands_ArriveInTheOrderTheyWerePosted)
{
	mailbox.post(Command::KeepAliveOn, peer);
	mailbox.post(Command::Reconfigure);
	mailbox.post(Command::KeepAliveOff, peer);

	mailbox.drain(work);

	ASSERT_EQ(work.commands.size(), 3u);
	EXPECT_EQ(work.commands[0].command, Command::KeepAliveOn);
	EXPECT_EQ(work.commands[0].peer, peer);
	EXPECT_EQ(work.commands[1].command, Command::Reconfigure);
	EXPECT_EQ(work.commands[2].command, Command::KeepAliveOff);
}


TEST_F(MailboxTest, Messages_WaitUntilEarlierCommandsWereTaken)
{
	mailbox.push(peer, Lane::Application, mail(1));
	mailbox.drain(work);

	mailbox.post(Command::KeepAliveOn, peer);

	EXPECT_TRUE(take().empty()) << "A command that was posted before the hand-over has to be carried out first";
	EXPECT_TRUE(stillWaiting);

	mailbox.drain(work);
	EXPECT_EQ(work.commands.size(), 1u);
	EXPECT_EQ(work.ready, (std::vector<net::SocketAddress>{peer})) << "The peer is offered again together with the command";

	EXPECT_EQ(take(), (Taken{{Lane::Application, 1}}));
}


// ---------------------------------------------------------------------------
// Waiting for room
// ---------------------------------------------------------------------------

TEST_F(MailboxTest, PushWithTimeout_WaitsForRoom)
{
	fillApplicationLane();

	auto pushed = std::async(std::launch::async, [this] { return mailbox.push(peer, Lane::Application, mail(9), 5s); });
	EXPECT_EQ(pushed.wait_for(50ms), std::future_status::timeout);

	EXPECT_EQ(take(1).size(), 1u);

	ASSERT_EQ(pushed.wait_for(2s), std::future_status::ready) << "Room in the lane must wake the waiting push";
	EXPECT_EQ(pushed.get(), Push::Queued);
}


TEST_F(MailboxTest, PushWithTimeout_GivesUpWhenNoRoomAppears)
{
	fillApplicationLane();

	const auto started = std::chrono::steady_clock::now();
	EXPECT_EQ(mailbox.push(peer, Lane::Application, mail(9), 100ms), Push::Full);
	EXPECT_GE(std::chrono::steady_clock::now() - started, 100ms);
}


TEST_F(MailboxTest, PushWithTimeout_EndsWhenThePeerIsClosed)
{
	fillApplicationLane();

	auto pushed = std::async(std::launch::async, [this] { return mailbox.push(peer, Lane::Application, mail(9), 5s); });
	std::this_thread::sleep_for(50ms);
	mailbox.close(peer);

	ASSERT_EQ(pushed.wait_for(2s), std::future_status::ready);
	EXPECT_EQ(pushed.get(), Push::Closed);
}


TEST_F(MailboxTest, PushWithTimeout_EndsWithAReset)
{
	fillApplicationLane();

	auto pushed = std::async(std::launch::async, [this] { return mailbox.push(peer, Lane::Application, mail(9), 5s); });
	std::this_thread::sleep_for(50ms);
	mailbox.reset();

	ASSERT_EQ(pushed.wait_for(2s), std::future_status::ready);
	EXPECT_EQ(pushed.get(), Push::Closed) << "The message belonged to what was reset, although there is room now";
}


TEST_F(MailboxTest, PushWithTimeout_EndsWhenTheLoopStops)
{
	fillApplicationLane();

	auto pushed = std::async(std::launch::async, [this] { return mailbox.push(peer, Lane::Application, mail(9), 5s); });
	std::this_thread::sleep_for(50ms);
	mailbox.setRunning(false);

	ASSERT_EQ(pushed.wait_for(2s), std::future_status::ready);
	EXPECT_EQ(pushed.get(), Push::Full);

	const auto started = std::chrono::steady_clock::now();
	EXPECT_EQ(mailbox.push(peer, Lane::Application, mail(9), 5s), Push::Full);
	EXPECT_LT(std::chrono::steady_clock::now() - started, 1s) << "Without a running loop nobody waits";
}


// ---------------------------------------------------------------------------
// Flush
// ---------------------------------------------------------------------------

TEST_F(MailboxTest, Flush_WaitsForEverythingAcceptedBeforeIt)
{
	EXPECT_TRUE(mailbox.flush(peer, 0ms)) << "Nothing was sent";

	mailbox.push(peer, Lane::Application, mail(1));
	EXPECT_FALSE(mailbox.flush(peer, 20ms)) << "Still in the mailbox";
	EXPECT_FALSE(mailbox.settle(peer)) << "The link may be idle, the mailbox is not";

	take();
	EXPECT_FALSE(mailbox.flush(peer, 20ms)) << "Handed to the link, not acknowledged";

	auto flushed = std::async(std::launch::async, [this] { return mailbox.flush(peer, 5s); });
	std::this_thread::sleep_for(50ms);
	EXPECT_TRUE(mailbox.settle(peer));

	ASSERT_EQ(flushed.wait_for(2s), std::future_status::ready);
	EXPECT_TRUE(flushed.get());
}


TEST_F(MailboxTest, Flush_IgnoresUnreliableMessages)
{
	mailbox.push(peer, Lane::Unreliable, mail(1));
	EXPECT_TRUE(mailbox.flush(peer, 0ms));
}


TEST_F(MailboxTest, Flush_EndsWhenTheLoopStops)
{
	mailbox.push(peer, Lane::Application, mail(1));

	auto flushed = std::async(std::launch::async, [this] { return mailbox.flush(peer, 5s); });
	std::this_thread::sleep_for(50ms);
	mailbox.setRunning(false);

	ASSERT_EQ(flushed.wait_for(2s), std::future_status::ready);
	EXPECT_FALSE(flushed.get()) << "Nothing was acknowledged";
}


// ---------------------------------------------------------------------------
// Dropping, closing, resetting
// ---------------------------------------------------------------------------

TEST_F(MailboxTest, DropApplication_ClearsAtOnceAndKeepsControl)
{
	fillApplicationLane();
	mailbox.push(peer, Lane::Unreliable, mail(7));
	mailbox.push(peer, Lane::Control, mail(8));

	mailbox.dropApplication(peer);
	EXPECT_EQ(mailbox.push(peer, Lane::Application, mail(9)), Push::Queued) << "The lane is empty the moment the call returns";

	mailbox.drain(work);
	ASSERT_EQ(work.commands.size(), 1u);
	EXPECT_EQ(work.commands[0].command, Command::DropApplication) << "The link has to drop what it already took";

	EXPECT_EQ(take(), (Taken{{Lane::Control, 8}, {Lane::Application, 9}}));
}


TEST_F(MailboxTest, Close_ForgetsThePeerAndErasesItsLink)
{
	mailbox.push(peer, Lane::Application, mail(1));
	mailbox.close(peer);

	EXPECT_EQ(mailbox.push(peer, Lane::Application, mail(2)), Push::Closed);

	mailbox.drain(work);
	ASSERT_EQ(work.commands.size(), 1u);
	EXPECT_EQ(work.commands[0].command, Command::EraseLink);
	EXPECT_EQ(work.commands[0].peer, peer);
	EXPECT_TRUE(take().empty());

	mailbox.open(peer);
	EXPECT_EQ(mailbox.push(peer, Lane::Application, mail(3)), Push::Queued);
	EXPECT_EQ(take(), (Taken{{Lane::Application, 3}})) << "A peer that is opened again starts empty";
}


TEST_F(MailboxTest, Reset_DropsEverythingAndKeepsThePeersOpen)
{
	mailbox.push(peer, Lane::Application, mail(1));
	mailbox.push(peer, Lane::Control, mail(2));

	mailbox.reset();

	EXPECT_TRUE(mailbox.flush(peer, 0ms)) << "Nothing is pending anymore";

	mailbox.drain(work);
	ASSERT_EQ(work.commands.size(), 1u);
	EXPECT_EQ(work.commands[0].command, Command::ResetLinks);
	EXPECT_TRUE(work.ready.empty());
	EXPECT_TRUE(take().empty());

	EXPECT_EQ(mailbox.push(peer, Lane::Application, mail(3)), Push::Queued);
}

} // namespace ChannelTests
