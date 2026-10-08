#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

#include "Channel/Mailbox.h"

using namespace netlink;
using namespace netlink::channel;
using namespace std::chrono_literals;


namespace ChannelTests
{

using netlink::channel::Lane;

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
	PeerId			   peer{42};
	Mailbox::Work	   work;
};


TEST_F(MailboxTest, Messages_AreTakenInOrderPerLane)
{
	EXPECT_EQ(mailbox.push(peer, Lane::Reliable, mail(1)), Push::Queued);
	EXPECT_EQ(mailbox.push(peer, Lane::Media, mail(2)), Push::Queued);
	EXPECT_EQ(mailbox.push(peer, Lane::Reliable, mail(3)), Push::Queued);
	EXPECT_EQ(mailbox.push(peer, Lane::Bulk, mail(5)), Push::Queued);

	EXPECT_EQ(takeAll(Lane::Reliable), (std::vector<uint32_t>{1, 3})) << "Every lane in the order it was filled";
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
	EXPECT_EQ(mailbox.stats(peer)->mediaDropped, 3u);
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
	const PeerId stranger{7};

	EXPECT_EQ(mailbox.push(stranger, Lane::Reliable, mail(1)), Push::Closed);
	EXPECT_FALSE(mailbox.take(stranger, Lane::Reliable).has_value());
	EXPECT_TRUE(mailbox.flush(stranger, 1s)) << "Nothing is pending for a peer that is not there";
}


TEST_F(MailboxTest, Doorbell_RingsWhenWorkArrivesAndNoneWasWaiting)
{
	mailbox.push(peer, Lane::Reliable, mail(1));
	mailbox.push(peer, Lane::Reliable, mail(2));
	mailbox.post(Command::Connect, peer);
	EXPECT_EQ(rings.load(), 1) << "The I/O thread was told already";

	mailbox.drain(work);
	EXPECT_EQ(rings.load(), 1);

	mailbox.post(Command::Disconnect, peer);
	EXPECT_EQ(rings.load(), 2) << "After a drain the next work rings again";
}


TEST_F(MailboxTest, Drain_ListsAPeerWithNewMessagesOnce)
{
	mailbox.push(peer, Lane::Reliable, mail(1));
	mailbox.push(peer, Lane::Bulk, mail(2));

	mailbox.drain(work);
	EXPECT_EQ(work.ready, (std::vector<PeerId>{peer}));
	EXPECT_TRUE(work.commands.empty());

	mailbox.drain(work);
	EXPECT_TRUE(work.ready.empty());

	mailbox.push(peer, Lane::Reliable, mail(3));
	mailbox.drain(work);
	EXPECT_EQ(work.ready, (std::vector<PeerId>{peer}));
}


TEST_F(MailboxTest, Commands_ArriveInTheOrderTheyWerePosted)
{
	mailbox.post(Command::Connect, peer);
	mailbox.post(Command::Announce);
	mailbox.post(Command::Disconnect, peer);

	mailbox.drain(work);

	ASSERT_EQ(work.commands.size(), 3u);
	EXPECT_EQ(work.commands[0].command, Command::Connect);
	EXPECT_EQ(work.commands[0].peer, peer);
	EXPECT_EQ(work.commands[1].command, Command::Announce);
	EXPECT_EQ(work.commands[2].command, Command::Disconnect);
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


TEST_F(MailboxTest, PushWithTimeout_EndsWhenTheLoopStops)
{
	fillReliableLane();

	auto pushed = std::async(std::launch::async, [this] { return mailbox.push(peer, Lane::Reliable, mail(9), 5s); });
	std::this_thread::sleep_for(50ms);
	mailbox.setRunning(false);

	ASSERT_EQ(pushed.wait_for(2s), std::future_status::ready);
	EXPECT_EQ(pushed.get(), Push::Stopped);

	const auto started = std::chrono::steady_clock::now();
	EXPECT_EQ(mailbox.push(peer, Lane::Reliable, mail(9), 5s), Push::Stopped);
	EXPECT_LT(std::chrono::steady_clock::now() - started, 1s) << "Without a running loop nobody waits";
	EXPECT_EQ(mailbox.push(peer, Lane::Bulk, mail(9)), Push::Stopped) << "... and nothing is taken";
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
// Sessions
// ---------------------------------------------------------------------------

TEST_F(MailboxTest, Close_ForgetsThePeer)
{
	mailbox.push(peer, Lane::Reliable, mail(1));
	mailbox.close(peer);

	EXPECT_EQ(mailbox.push(peer, Lane::Reliable, mail(2)), Push::Closed);
	EXPECT_TRUE(takeAll(Lane::Reliable).empty());
	EXPECT_TRUE(mailbox.flush(peer, 0ms)) << "Nothing is pending anymore";

	mailbox.drain(work);
	EXPECT_TRUE(work.ready.empty());

	mailbox.open(peer);
	EXPECT_EQ(mailbox.push(peer, Lane::Reliable, mail(3)), Push::Queued);
	EXPECT_EQ(takeAll(Lane::Reliable), (std::vector<uint32_t>{3})) << "A session that is opened again starts empty";
}


TEST_F(MailboxTest, SealedSession_TakesNothingNewButKeepsWhatWaits)
{
	mailbox.push(peer, Lane::Reliable, mail(1));
	mailbox.push(peer, Lane::Bulk, mail(2));
	mailbox.push(peer, Lane::Media, mail(3));

	mailbox.seal(peer);

	EXPECT_EQ(mailbox.push(peer, Lane::Reliable, mail(4)), Push::Closed);
	EXPECT_EQ(takeAll(Lane::Reliable), (std::vector<uint32_t>{1})) << "What was accepted is still sent";
	EXPECT_EQ(takeAll(Lane::Bulk), (std::vector<uint32_t>{2}));
	EXPECT_TRUE(takeAll(Lane::Media).empty()) << "Media is not worth waiting for";
	EXPECT_TRUE(mailbox.connected().empty());
}


TEST_F(MailboxTest, Seal_EndsAWaitingPush)
{
	fillReliableLane();

	auto pushed = std::async(std::launch::async, [this] { return mailbox.push(peer, Lane::Reliable, mail(9), 5s); });
	std::this_thread::sleep_for(50ms);
	mailbox.seal(peer);

	ASSERT_EQ(pushed.wait_for(2s), std::future_status::ready);
	EXPECT_EQ(pushed.get(), Push::Closed);
}


TEST_F(MailboxTest, PushToAll_ReachesEveryOpenSessionWithRoom)
{
	const PeerId second{43};
	const PeerId sealed{44};
	mailbox.open(second);
	mailbox.open(sealed);
	mailbox.seal(sealed);
	fillReliableLane();

	const auto message = mail(7);
	EXPECT_EQ(mailbox.pushToAll(Lane::Reliable, message), 1u) << "Not the full lane, not the sealed session";
	EXPECT_EQ(mailbox.take(second, Lane::Reliable)->body, message.body) << "One body for all of them";
	EXPECT_EQ(mailbox.pushToAll(Lane::Media, mail(8, internal::MaxMediaPayload + 1)), 0u);
}


TEST_F(MailboxTest, PublishedState_IsWhatTheEngineLastSaid)
{
	using Session = Mailbox::Session;
	const PeerId other{43};

	EXPECT_EQ(mailbox.session(peer), Session::Connected);
	EXPECT_EQ(mailbox.session(other), Session::None);

	mailbox.publishSession(other, Session::Requested);
	EXPECT_EQ(mailbox.session(other), Session::Requested);
	EXPECT_EQ(mailbox.connected(), (std::vector<PeerId>{peer}));

	mailbox.close(other);
	EXPECT_EQ(mailbox.session(other), Session::None);

	mailbox.publishDiscovered({PeerInfo{.id = other, .displayName = "other"}});
	EXPECT_TRUE(mailbox.isDiscovered(other));
	EXPECT_FALSE(mailbox.isDiscovered(peer));
	ASSERT_EQ(mailbox.discovered().size(), 1u);
}


TEST_F(MailboxTest, Stats_AddWhatWaitsToWhatTheEngineCounted)
{
	EXPECT_FALSE(mailbox.stats(PeerId{7}).has_value());

	mailbox.push(peer, Lane::Reliable, mail(1));
	mailbox.push(peer, Lane::Media, mail(2, 30));
	mailbox.publishStats(peer, PeerStats{.bytesSent = 500});

	const auto stats = mailbox.stats(peer);
	ASSERT_TRUE(stats.has_value());
	EXPECT_EQ(stats->bytesSent, 500u);
	EXPECT_EQ(stats->bytesQueued, MessageSize + 30);
}

} // namespace ChannelTests
