#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
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
	using Push	  = Mailbox::Push;
	using Command						= Mailbox::Command;

	// Room for three messages of this size in an acknowledged lane
	static constexpr size_t MessageSize = 100;
	static constexpr size_t QueueBytes	= 3 * MessageSize;

	void					SetUp() override
	{
		mailbox.setQueueBytes(QueueBytes);
		mailbox.open(peer);
		mailbox.setRunning(true);
	}

	static Mailbox::Mail  mail(const uint32_t tag, const size_t size = MessageSize) { return {.tag = tag, .body = std::make_shared<const std::vector<uint8_t>>(size)}; }

	// The tags of everything that waits in the lane, taken in order
	std::vector<uint32_t> takeAll(const Lane lane)
	{
		std::vector<uint32_t> tags;
		while (auto taken = mailbox.take(peer, lane))
			tags.push_back(taken->tag);
		return tags;
	}

	void fillReliableLane()
	{
		for (uint32_t i = 0; i < 3; ++i)
			ASSERT_EQ(mailbox.push(peer, Lane::Reliable, mail(i)), Push::Queued);
	}

	std::atomic<int>   rings{0};
	Mailbox			   mailbox{[this] { ++rings; }};
	net::SocketAddress peer{ipv4("10.0.0.2"), 4000};
	Mailbox::Work	   work;
};


TEST_F(MailboxTest, Messages_AreTakenInOrderPerLane)
{
	EXPECT_EQ(mailbox.push(peer, Lane::Reliable, mail(1)), Push::Queued);
	EXPECT_EQ(mailbox.push(peer, Lane::Media, mail(2)), Push::Queued);
	EXPECT_EQ(mailbox.push(peer, Lane::Reliable, mail(3)), Push::Queued);
	EXPECT_EQ(mailbox.push(peer, Lane::Control, mail(4)), Push::Queued);
	EXPECT_EQ(mailbox.push(peer, Lane::Bulk, mail(5)), Push::Queued);

	EXPECT_EQ(takeAll(Lane::Reliable), (std::vector<uint32_t>{1, 3})) << "Every lane in the order it was filled";
	EXPECT_EQ(takeAll(Lane::Control), (std::vector<uint32_t>{4}));
	EXPECT_EQ(takeAll(Lane::Bulk), (std::vector<uint32_t>{5}));
	EXPECT_EQ(takeAll(Lane::Media), (std::vector<uint32_t>{2}));
	EXPECT_FALSE(mailbox.take(peer, Lane::Reliable).has_value()) << "Handed over once";
}


TEST_F(MailboxTest, FullLane_RefusesTheNewestMessage)
{
	fillReliableLane();

	EXPECT_EQ(mailbox.push(peer, Lane::Reliable, mail(9)), Push::Full);
	EXPECT_EQ(mailbox.push(peer, Lane::Reliable, mail(9, 1)), Push::Full) << "Not even one more byte";
	EXPECT_EQ(mailbox.push(peer, Lane::Bulk, mail(9)), Push::Queued) << "Every lane has room of its own";
	EXPECT_EQ(mailbox.push(peer, Lane::Control, mail(9)), Push::Queued);
}


TEST_F(MailboxTest, Room_IsCountedInBytes)
{
	EXPECT_EQ(mailbox.push(peer, Lane::Reliable, mail(1, QueueBytes - 10)), Push::Queued);
	EXPECT_EQ(mailbox.push(peer, Lane::Reliable, mail(2, 10)), Push::Queued);
	EXPECT_EQ(mailbox.push(peer, Lane::Reliable, mail(3, 1)), Push::Full);

	ASSERT_TRUE(mailbox.take(peer, Lane::Reliable).has_value());
	EXPECT_EQ(mailbox.push(peer, Lane::Reliable, mail(3, QueueBytes - 10)), Push::Queued) << "What was taken made room";
}


TEST_F(MailboxTest, MessageLargerThanTheLane_IsTakenWhenNothingElseWaits)
{
	EXPECT_EQ(mailbox.push(peer, Lane::Reliable, mail(1, 10 * QueueBytes)), Push::Queued) << "Otherwise it could never be sent";
	EXPECT_EQ(mailbox.push(peer, Lane::Reliable, mail(2, 1)), Push::Full);
}


TEST_F(MailboxTest, Media_DropsItsOldestMessageInsteadOfRefusing)
{
	for (uint32_t i = 0; i < MediaQueueMessages + 3; ++i)
		EXPECT_EQ(mailbox.push(peer, Lane::Media, mail(i)), Push::Queued);

	const auto tags = takeAll(Lane::Media);

	ASSERT_EQ(tags.size(), MediaQueueMessages);
	EXPECT_EQ(tags.front(), 3u) << "The three oldest made room";
	EXPECT_EQ(tags.back(), MediaQueueMessages + 2);
}


TEST_F(MailboxTest, OversizedMessages_AreRefused)
{
	mailbox.setQueueBytes(DefaultSendQueueBytes);

	EXPECT_EQ(mailbox.push(peer, Lane::Reliable, mail(1, internal::MaxMessagePayload)), Push::Queued);
	EXPECT_EQ(mailbox.push(peer, Lane::Bulk, mail(1, internal::MaxMessagePayload + 1)), Push::TooLarge);
	EXPECT_EQ(mailbox.push(peer, Lane::Media, mail(1, internal::MaxMediaPayload)), Push::Queued);
	EXPECT_EQ(mailbox.push(peer, Lane::Media, mail(1, internal::MaxMediaPayload + 1)), Push::TooLarge) << "Media messages are small";
}


TEST_F(MailboxTest, UnknownPeer_IsClosed)
{
	const net::SocketAddress stranger{ipv4("10.0.0.9"), 1};

	EXPECT_EQ(mailbox.push(stranger, Lane::Reliable, mail(1)), Push::Closed);
	EXPECT_FALSE(mailbox.take(stranger, Lane::Reliable).has_value());
	EXPECT_TRUE(mailbox.flush(stranger, 1s)) << "Nothing is pending for a peer that is not there";
}


TEST_F(MailboxTest, Doorbell_RingsWhenWorkArrivesAndNoneWasWaiting)
{
	mailbox.push(peer, Lane::Reliable, mail(1));
	mailbox.push(peer, Lane::Reliable, mail(2));
	mailbox.post(Command::KeepAliveOn, peer);
	EXPECT_EQ(rings.load(), 1) << "The I/O thread was told already";

	mailbox.drain(work);
	EXPECT_EQ(rings.load(), 1);

	mailbox.post(Command::KeepAliveOff, peer);
	EXPECT_EQ(rings.load(), 2) << "After a drain the next work rings again";
}


TEST_F(MailboxTest, Drain_ListsAPeerWithNewMessagesOnce)
{
	mailbox.push(peer, Lane::Reliable, mail(1));
	mailbox.push(peer, Lane::Control, mail(2));

	mailbox.drain(work);
	EXPECT_EQ(work.ready, (std::vector<net::SocketAddress>{peer}));
	EXPECT_TRUE(work.commands.empty());

	mailbox.drain(work);
	EXPECT_TRUE(work.ready.empty());

	mailbox.push(peer, Lane::Reliable, mail(3));
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
	mailbox.push(peer, Lane::Reliable, mail(1));
	mailbox.drain(work);

	mailbox.post(Command::KeepAliveOn, peer);

	EXPECT_FALSE(mailbox.take(peer, Lane::Reliable).has_value()) << "A command that was posted before the hand-over has to be carried out first";

	mailbox.drain(work);
	EXPECT_EQ(work.commands.size(), 1u);
	EXPECT_EQ(work.ready, (std::vector<net::SocketAddress>{peer})) << "The peer is offered again together with the command";

	EXPECT_EQ(takeAll(Lane::Reliable), (std::vector<uint32_t>{1}));
}


// ---------------------------------------------------------------------------
// Waiting for room
// ---------------------------------------------------------------------------

TEST_F(MailboxTest, PushWithTimeout_WaitsForRoom)
{
	fillReliableLane();

	auto pushed = std::async(std::launch::async, [this] { return mailbox.push(peer, Lane::Reliable, mail(9), 5s); });
	EXPECT_EQ(pushed.wait_for(50ms), std::future_status::timeout);

	ASSERT_TRUE(mailbox.take(peer, Lane::Reliable).has_value());

	ASSERT_EQ(pushed.wait_for(2s), std::future_status::ready) << "Room in the lane must wake the waiting push";
	EXPECT_EQ(pushed.get(), Push::Queued);
}


TEST_F(MailboxTest, PushWithTimeout_GivesUpWhenNoRoomAppears)
{
	fillReliableLane();

	const auto started = std::chrono::steady_clock::now();
	EXPECT_EQ(mailbox.push(peer, Lane::Reliable, mail(9), 100ms), Push::Full);
	EXPECT_GE(std::chrono::steady_clock::now() - started, 100ms);
}


TEST_F(MailboxTest, PushWithTimeout_EndsWhenThePeerIsClosed)
{
	fillReliableLane();

	auto pushed = std::async(std::launch::async, [this] { return mailbox.push(peer, Lane::Reliable, mail(9), 5s); });
	std::this_thread::sleep_for(50ms);
	mailbox.close(peer);

	ASSERT_EQ(pushed.wait_for(2s), std::future_status::ready);
	EXPECT_EQ(pushed.get(), Push::Closed);
}


TEST_F(MailboxTest, PushWithTimeout_EndsWithAReset)
{
	fillReliableLane();

	auto pushed = std::async(std::launch::async, [this] { return mailbox.push(peer, Lane::Reliable, mail(9), 5s); });
	std::this_thread::sleep_for(50ms);
	mailbox.reset();

	ASSERT_EQ(pushed.wait_for(2s), std::future_status::ready);
	EXPECT_EQ(pushed.get(), Push::Closed) << "The message belonged to what was reset, although there is room now";
}


TEST_F(MailboxTest, PushWithTimeout_EndsWhenTheLoopStops)
{
	fillReliableLane();

	auto pushed = std::async(std::launch::async, [this] { return mailbox.push(peer, Lane::Reliable, mail(9), 5s); });
	std::this_thread::sleep_for(50ms);
	mailbox.setRunning(false);

	ASSERT_EQ(pushed.wait_for(2s), std::future_status::ready);
	EXPECT_EQ(pushed.get(), Push::Full);

	const auto started = std::chrono::steady_clock::now();
	EXPECT_EQ(mailbox.push(peer, Lane::Reliable, mail(9), 5s), Push::Full);
	EXPECT_LT(std::chrono::steady_clock::now() - started, 1s) << "Without a running loop nobody waits";
}


// ---------------------------------------------------------------------------
// Flush
// ---------------------------------------------------------------------------

TEST_F(MailboxTest, Flush_WaitsForEverythingAcceptedBeforeIt)
{
	EXPECT_TRUE(mailbox.flush(peer, 0ms)) << "Nothing was sent";

	mailbox.push(peer, Lane::Reliable, mail(1));
	EXPECT_FALSE(mailbox.flush(peer, 20ms)) << "Still in the mailbox";
	EXPECT_FALSE(mailbox.settle(peer)) << "The link may be idle, the mailbox is not";

	ASSERT_TRUE(mailbox.take(peer, Lane::Reliable).has_value());
	EXPECT_FALSE(mailbox.flush(peer, 20ms)) << "Handed to the link, not acknowledged";

	auto flushed = std::async(std::launch::async, [this] { return mailbox.flush(peer, 5s); });
	std::this_thread::sleep_for(50ms);
	EXPECT_TRUE(mailbox.settle(peer));

	ASSERT_EQ(flushed.wait_for(2s), std::future_status::ready);
	EXPECT_TRUE(flushed.get());
}


TEST_F(MailboxTest, Flush_IgnoresMedia)
{
	mailbox.push(peer, Lane::Media, mail(1));
	EXPECT_TRUE(mailbox.flush(peer, 0ms)) << "Nothing ever acknowledges it";
	EXPECT_TRUE(mailbox.settle(peer));
}


TEST_F(MailboxTest, Flush_EndsWhenTheLoopStops)
{
	mailbox.push(peer, Lane::Reliable, mail(1));

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
	fillReliableLane();
	mailbox.push(peer, Lane::Bulk, mail(6));
	mailbox.push(peer, Lane::Media, mail(7));
	mailbox.push(peer, Lane::Control, mail(8));

	mailbox.dropApplication(peer);
	EXPECT_EQ(mailbox.push(peer, Lane::Reliable, mail(9, QueueBytes)), Push::Queued) << "The lane is empty the moment the call returns";

	mailbox.drain(work);
	ASSERT_EQ(work.commands.size(), 1u);
	EXPECT_EQ(work.commands[0].command, Command::DropApplication) << "Whoever waits for what was dropped has to be told";

	EXPECT_EQ(takeAll(Lane::Control), (std::vector<uint32_t>{8}));
	EXPECT_EQ(takeAll(Lane::Reliable), (std::vector<uint32_t>{9}));
	EXPECT_TRUE(takeAll(Lane::Bulk).empty());
	EXPECT_TRUE(takeAll(Lane::Media).empty());
}


TEST_F(MailboxTest, Close_ForgetsThePeerAndErasesItsLink)
{
	mailbox.push(peer, Lane::Reliable, mail(1));
	mailbox.close(peer);

	EXPECT_EQ(mailbox.push(peer, Lane::Reliable, mail(2)), Push::Closed);

	mailbox.drain(work);
	ASSERT_EQ(work.commands.size(), 1u);
	EXPECT_EQ(work.commands[0].command, Command::EraseLink);
	EXPECT_EQ(work.commands[0].peer, peer);
	EXPECT_TRUE(takeAll(Lane::Reliable).empty());

	mailbox.open(peer);
	EXPECT_EQ(mailbox.push(peer, Lane::Reliable, mail(3)), Push::Queued);
	EXPECT_EQ(takeAll(Lane::Reliable), (std::vector<uint32_t>{3})) << "A peer that is opened again starts empty";
}


TEST_F(MailboxTest, Reset_DropsEverythingAndKeepsThePeersOpen)
{
	mailbox.push(peer, Lane::Reliable, mail(1));
	mailbox.push(peer, Lane::Control, mail(2));

	mailbox.reset();

	EXPECT_TRUE(mailbox.flush(peer, 0ms)) << "Nothing is pending anymore";

	mailbox.drain(work);
	ASSERT_EQ(work.commands.size(), 1u);
	EXPECT_EQ(work.commands[0].command, Command::ResetLinks);
	EXPECT_TRUE(work.ready.empty());
	EXPECT_TRUE(takeAll(Lane::Reliable).empty());

	EXPECT_EQ(mailbox.push(peer, Lane::Reliable, mail(3)), Push::Queued);
}

} // namespace ChannelTests
