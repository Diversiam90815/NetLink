#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "EngineHarness.h"
#include "FakeDatagramNetwork.h"
#include "LossyDatagramSocket.h"
#include "SpyDatagramSocket.h"
#include "TestIp.h"

using namespace netlink;
using namespace std::chrono_literals;
using FakeNet::ThreadedEngine;
using FakeNet::waitUntilTrue;


namespace EngineTests
{

// Short timers so loss detection runs in test time
static EngineConfig fastConfig()
{
	EngineConfig config;
	config.appId			   = "engine-tests";
	config.appVersion		   = "1.0";
	config.timings.initialRto  = 50ms;
	config.timings.minRto	   = 10ms;
	config.timings.maxRto	   = 100ms;
	config.timings.peerTimeout = 500ms;
	config.timings.keepAlive   = 50ms;
	return config;
}


// Two engines on a network in memory, each with its own threads. Not connected yet.
class UnconnectedNetworkEngineTest : public ::testing::Test
{
protected:
	static constexpr const char *AddressA = "10.0.0.1";
	static constexpr const char *AddressB = "10.0.0.2";

	void						 SetUp() override { start(network->factory(AddressA), network->factory(AddressB)); }

	void						 start(net::DatagramSocketFactory factoryA, net::DatagramSocketFactory factoryB, const EngineConfig &config = fastConfig())
	{
		b.reset();
		a.reset();

		a = make("pc-a", AddressA, std::move(factoryA), config);
		b = make("pc-b", AddressB, std::move(factoryB), config);
	}

	static std::unique_ptr<ThreadedEngine> make(const std::string &name, const std::string &ip, net::DatagramSocketFactory factory, EngineConfig config = fastConfig())
	{
		config.displayName = name;

		auto engine		   = std::make_unique<ThreadedEngine>(config, std::move(factory), ip);
		engine->start();
		return engine;
	}

	static std::vector<uint8_t> payload(size_t size, uint8_t seed)
	{
		std::vector<uint8_t> data(size);
		for (size_t i = 0; i < size; ++i)
			data[i] = static_cast<uint8_t>(seed + i * 7);
		return data;
	}

	static SendResult send(ThreadedEngine &from, const ThreadedEngine &to, const uint32_t type, std::vector<uint8_t> data, const Lane lane = Lane::Reliable,
						   const std::chrono::milliseconds timeout = {})
	{
		return from.engine.send(to.id(), type, std::move(data), lane, timeout);
	}

	static bool sent(ThreadedEngine &from, const ThreadedEngine &to, const uint32_t type, std::vector<uint8_t> data, const Lane lane = Lane::Reliable,
					 const std::chrono::milliseconds timeout = {})
	{
		return send(from, to, type, std::move(data), lane, timeout) == SendResult::Queued;
	}

	static bool endedWith(const ThreadedEngine &engine, const ThreadedEngine &peer, const DisconnectReason reason)
	{
		return engine.events.ended() == std::vector<FakeNet::Ended>{{peer.id(), reason}};
	}

	std::shared_ptr<FakeNet::FakeDatagramNetwork> network = FakeNet::FakeDatagramNetwork::create();
	std::shared_ptr<FakeNet::SocketUsage>		  usageA  = std::make_shared<FakeNet::SocketUsage>();

	std::unique_ptr<ThreadedEngine>				  a;
	std::unique_ptr<ThreadedEngine>				  b;
};


// ... with a session between them
class NetworkEngineTest : public UnconnectedNetworkEngineTest
{
protected:
	void SetUp() override
	{
		UnconnectedNetworkEngineTest::SetUp();
		ASSERT_TRUE(FakeNet::connect(*a, *b));
	}

	// Connects again, with everything A does to its socket recorded in usageA
	void spyOnA(const EngineConfig &config = fastConfig())
	{
		start(FakeNet::SpyDatagramSocket::wrap(network->factory(AddressA), usageA), network->factory(AddressB), config);
		ASSERT_TRUE(FakeNet::connect(*a, *b));
	}
};


// ---------------------------------------------------------------------------
// Discovery and sessions
// ---------------------------------------------------------------------------

TEST_F(UnconnectedNetworkEngineTest, Peers_DiscoverEachOtherWithNameAndAddress)
{
	ASSERT_TRUE(FakeNet::discover(*a, *b));

	const auto seenByA = a->events.discovered();
	ASSERT_EQ(seenByA.size(), 1u);
	EXPECT_EQ(seenByA[0].id, b->id());
	EXPECT_EQ(seenByA[0].displayName, "pc-b");
	EXPECT_EQ(seenByA[0].address, AddressB);
	EXPECT_EQ(seenByA[0].port, b->engine.localEndpoint().port) << "A peer is reached where its announcement came from";
	EXPECT_EQ(seenByA[0].appVersion, "1.0");

	ASSERT_EQ(a->engine.peers().size(), 1u);
	EXPECT_EQ(a->engine.peers()[0].id, b->id());
}


TEST_F(UnconnectedNetworkEngineTest, ConnectionRequest_NamesTheSender)
{
	EngineConfig asking = fastConfig();
	asking.autoAccept	= false;
	b					= make("pc-b", AddressB, network->factory(AddressB), asking);

	ASSERT_TRUE(FakeNet::discover(*a, *b));
	ASSERT_TRUE(a->engine.connect(b->id()));

	ASSERT_TRUE(waitUntilTrue([this] { return b->events.requests().size() == 1; }));
	EXPECT_EQ(b->events.requests()[0].id, a->id());
	EXPECT_EQ(b->events.requests()[0].displayName, "pc-a");
	EXPECT_TRUE(a->events.connected().empty()) << "Nobody answered yet";

	b->engine.accept(a->id());

	EXPECT_TRUE(waitUntilTrue([this] { return a->events.isConnectedTo(b->id()) && b->events.isConnectedTo(a->id()); }));
	EXPECT_EQ(a->engine.connectedPeers(), std::vector<PeerId>{b->id()});
	EXPECT_EQ(b->engine.connectedPeers(), std::vector<PeerId>{a->id()});
}


TEST_F(UnconnectedNetworkEngineTest, Decline_ReachesTheInitiator)
{
	EngineConfig asking = fastConfig();
	asking.autoAccept	= false;
	b					= make("pc-b", AddressB, network->factory(AddressB), asking);

	ASSERT_TRUE(FakeNet::discover(*a, *b));
	ASSERT_TRUE(a->engine.connect(b->id()));
	ASSERT_TRUE(waitUntilTrue([this] { return b->events.requests().size() == 1; }));

	b->engine.decline(a->id());

	ASSERT_TRUE(waitUntilTrue([this] { return !a->events.ended().empty() && !b->events.ended().empty(); }));
	EXPECT_TRUE(endedWith(*a, *b, DisconnectReason::Declined));
	EXPECT_TRUE(endedWith(*b, *a, DisconnectReason::Local)) << "Every request has one outcome, also the one that was declined here";
	EXPECT_TRUE(a->events.connected().empty());
	EXPECT_TRUE(a->engine.connectedPeers().empty());
}


TEST_F(UnconnectedNetworkEngineTest, Session_ConnectsAndDisconnectsOnBothSides)
{
	ASSERT_TRUE(FakeNet::connect(*a, *b));
	EXPECT_EQ(a->events.connected()[0].displayName, "pc-b");
	EXPECT_EQ(b->events.connected()[0].displayName, "pc-a");

	a->engine.disconnect(b->id());

	ASSERT_TRUE(waitUntilTrue([this] { return !a->events.ended().empty() && !b->events.ended().empty(); }));
	EXPECT_TRUE(endedWith(*a, *b, DisconnectReason::Local));
	EXPECT_TRUE(endedWith(*b, *a, DisconnectReason::Remote));
	EXPECT_EQ(send(*a, *b, 1, payload(4, 0)), SendResult::NotConnected);

	// ... and again
	ASSERT_TRUE(waitUntilTrue([this] { return a->engine.connect(b->id()); })) << "Once the old session is gone";
	EXPECT_TRUE(waitUntilTrue([this] { return a->events.connected().size() == 2 && b->events.connected().size() == 2; }));
}


TEST_F(UnconnectedNetworkEngineTest, SendToUnknownPeer_Fails)
{
	EXPECT_FALSE(a->engine.connect(PeerId{12345})) << "Only a discovered peer can be asked";
	EXPECT_EQ(send(*a, *b, 1, payload(4, 0)), SendResult::NotConnected);

	ASSERT_TRUE(FakeNet::discover(*a, *b));
	EXPECT_EQ(send(*a, *b, 1, payload(4, 0)), SendResult::NotConnected) << "Discovered is not connected";
	EXPECT_EQ(a->engine.broadcast(1, payload(4, 0), Lane::Reliable), 0u);
}


TEST_F(UnconnectedNetworkEngineTest, EngineWithoutAnAddress_StaysIdle)
{
	const auto lonely = std::make_unique<ThreadedEngine>(fastConfig(), network->factory("10.0.0.3"), "10.0.0.3");
	lonely->iface.clear();
	lonely->start();
	lonely->engine.setAnnouncing(true);

	std::this_thread::sleep_for(100ms);

	EXPECT_EQ(lonely->engine.localEndpoint().port, 0);
	EXPECT_TRUE(lonely->events.addresses().empty());
	EXPECT_FALSE(lonely->engine.connect(b->id()));
}


TEST_F(UnconnectedNetworkEngineTest, Rebind_WhileRunning_KeepsReceiving)
{
	// B's machine has a second address, and B moves to it
	const std::vector<FakeNet::Interface> interfaces{{.ip = ipv4("10.0.0.5")}, {.ip = ipv4("10.0.0.55")}};
	b = make("pc-b", "10.0.0.5", network->factory(interfaces));

	ASSERT_TRUE(FakeNet::connect(*a, *b));
	const auto oldEndpoint = b->engine.localEndpoint();

	b->iface.set("10.0.0.55");
	b->engine.checkInterface();

	ASSERT_TRUE(waitUntilTrue([this] { return b->events.addresses().size() == 2; })) << "The new address is reported";
	EXPECT_EQ(b->events.addresses().back(), "10.0.0.55");
	EXPECT_NE(b->engine.localEndpoint(), oldEndpoint);
	EXPECT_TRUE(endedWith(*b, *a, DisconnectReason::Local)) << "A session cannot move to another address";

	// A finds B again at its new address and can talk to it
	ASSERT_TRUE(waitUntilTrue(
		[this]
		{
			b->engine.setAnnouncing(true);
			const auto peers = a->engine.peers();
			return !peers.empty() && peers[0].address == "10.0.0.55";
		}));

	ASSERT_TRUE(waitUntilTrue([this] { return a->engine.connect(b->id()); }, 5s)) << "The session to the old address has to be given up first";
	ASSERT_TRUE(waitUntilTrue([this] { return b->events.connected().size() == 2; }));
	ASSERT_TRUE(sent(*a, *b, 7, payload(8, 1)));
	EXPECT_TRUE(waitUntilTrue([this] { return b->events.messageCount() == 1; }));
}


TEST_F(UnconnectedNetworkEngineTest, Stop_IsIdempotent)
{
	a->stop();
	EXPECT_NO_THROW(a->stop());
	EXPECT_FALSE(a->engine.isRunning());
	EXPECT_EQ(send(*a, *b, 1, payload(4, 0)), SendResult::NotRunning);
}


TEST_F(NetworkEngineTest, ForeignDatagrams_AreIgnored)
{
	auto intruder = network->factory("10.0.0.9")({ipv4("10.0.0.9"), 0}, {});
	ASSERT_TRUE(intruder.has_value());

	const auto		  target	  = b->engine.localEndpoint();
	const std::string oldProtocol = R"({"type":0,"name":"pc-x","IPv4":"10.0.0.9","sigPort":1})";
	static_cast<void>((*intruder)->sendTo(target, std::span(reinterpret_cast<const uint8_t *>(oldProtocol.data()), oldProtocol.size())));

	const std::vector<uint8_t> junk{0x4E, 0x4C, 0x01};
	static_cast<void>((*intruder)->sendTo(target, junk));

	// A beacon that is cut off, and one of another application
	const std::vector<uint8_t> shortBeacon{0x4E, 0x4C, 0x03, 0x03, 0x00, 0x01};
	static_cast<void>((*intruder)->sendTo(target, shortBeacon));
	static_cast<void>((*intruder)->sendTo(target, discovery::encodeBeacon({.instanceId = 99, .appIdHash = 1, .version = {1, 0}, .reply = false, .name = "pc-x"})));

	ASSERT_TRUE(sent(*a, *b, 5, payload(8, 1)));

	ASSERT_TRUE(waitUntilTrue([this] { return b->events.messageCount() == 1; }));
	EXPECT_EQ(b->events.messages()[0].from, a->id()) << "Datagrams of an older build or garbage must neither crash nor be routed";
	EXPECT_EQ(b->events.discovered().size(), 1u) << "... nor discovered";
}


TEST_F(UnconnectedNetworkEngineTest, DataWithoutASession_IsNeitherDeliveredNorAcknowledged)
{
	const auto usageB = std::make_shared<FakeNet::SocketUsage>();
	b				  = make("pc-b", AddressB, FakeNet::SpyDatagramSocket::wrap(network->factory(AddressB), usageB));
	ASSERT_TRUE(waitUntilTrue([this] { return b->engine.localEndpoint().port != 0; }));

	auto stranger = network->factory("10.0.0.9")({ipv4("10.0.0.9"), 0}, {});
	ASSERT_TRUE(stranger.has_value());

	const size_t		  sendsBefore = usageB->sends.load();
	const auto			  body		  = payload(8, 1);

	channel::PacketHeader header;
	header.flags	   = channel::PacketFlags::data(channel::Lane::Reliable);
	header.srcStreamID = 0x1234;
	header.dstStreamID = 0x5678;
	header.seq		   = 1;
	header.tag		   = 9;
	static_cast<void>((*stranger)->sendTo(b->engine.localEndpoint(), channel::encodePacket(header, body)));

	// A first Control packet that is no Hello either
	header.flags	   = channel::PacketFlags::data(channel::Lane::Control);
	header.dstStreamID = 0;
	static_cast<void>((*stranger)->sendTo(b->engine.localEndpoint(), channel::encodePacket(header, body)));

	std::this_thread::sleep_for(200ms);

	EXPECT_EQ(b->events.messageCount(), 0u);
	EXPECT_TRUE(b->events.requests().empty());
	EXPECT_EQ(usageB->sends.load(), sendsBefore) << "Whoever has no session gets no answer, and no state is kept for it";
}


// ---------------------------------------------------------------------------
// Application messages
// ---------------------------------------------------------------------------

TEST_F(NetworkEngineTest, ApplicationMessages_ArriveInOrderWithTypeAndSender)
{
	for (uint32_t i = 0; i < 50; ++i)
		ASSERT_TRUE(sent(*a, *b, i, payload(i, static_cast<uint8_t>(i))));

	ASSERT_TRUE(waitUntilTrue([this] { return b->events.messageCount() == 50; }));

	const auto messages = b->events.messages();
	for (uint32_t i = 0; i < 50; ++i)
	{
		EXPECT_EQ(messages[i].from, a->id());
		EXPECT_EQ(messages[i].lane, Lane::Reliable);
		EXPECT_EQ(messages[i].type, i);
		EXPECT_EQ(messages[i].data, payload(i, static_cast<uint8_t>(i)));
	}
}


TEST_F(NetworkEngineTest, SendingInBothDirectionsAtOnce_KeepsTheOrder)
{
	// Each engine is sent to from a thread of its own while its I/O thread receives, acknowledges and sends as well
	constexpr uint32_t Messages = 3000;

	const auto		   sendAll	= [](ThreadedEngine &from, const ThreadedEngine &to)
	{
		for (uint32_t i = 0; i < Messages; ++i)
		{
			if (!sent(from, to, i, payload(64, static_cast<uint8_t>(i)), Lane::Reliable, 10s))
				return false;
		}
		return true;
	};

	auto fromA = std::async(std::launch::async, [&] { return sendAll(*a, *b); });
	auto fromB = std::async(std::launch::async, [&] { return sendAll(*b, *a); });

	ASSERT_TRUE(fromA.get());
	ASSERT_TRUE(fromB.get());
	ASSERT_TRUE(waitUntilTrue([this] { return a->events.messageCount() == Messages && b->events.messageCount() == Messages; }, 20s))
		<< a->events.messageCount() << " at A, " << b->events.messageCount() << " at B";

	for (auto *engine : {a.get(), b.get()})
	{
		const auto messages = engine->events.messages();
		for (uint32_t i = 0; i < Messages; ++i)
			ASSERT_EQ(messages[i].type, i) << "Order broken at " << i;
	}
}


TEST_F(NetworkEngineTest, LargeMessage_IsFragmentedAndReassembled)
{
	const auto big = payload(size_t{512} * 1024, 3);

	ASSERT_TRUE(sent(*a, *b, 77, big));
	ASSERT_TRUE(waitUntilTrue([this] { return b->events.messageCount() == 1; }, 10s));

	const auto messages = b->events.messages();
	EXPECT_EQ(messages[0].type, 77u);
	EXPECT_EQ(messages[0].data, big);
}


TEST_F(NetworkEngineTest, MediaMessages_AreDelivered)
{
	for (uint32_t i = 0; i < 10; ++i)
		ASSERT_TRUE(sent(*a, *b, i, payload(8, 0), Lane::Media));

	EXPECT_TRUE(waitUntilTrue([this] { return b->events.messageCount() == 10; })) << "Without loss every Media message arrives";
	EXPECT_EQ(b->events.messages()[0].lane, Lane::Media);

	const auto large = payload(40'000, 5);
	ASSERT_TRUE(sent(*a, *b, 11, large, Lane::Media)) << "Larger ones are sent in several datagrams";
	ASSERT_TRUE(waitUntilTrue([this] { return b->events.messageCount() == 11; }));
	EXPECT_EQ(b->events.messages().back().data, large);

	EXPECT_EQ(send(*a, *b, 1, payload(MaxMediaMessageSize + 1, 0), Lane::Media), SendResult::TooLarge) << "... up to a limit";
}


TEST_F(NetworkEngineTest, Lanes_KeepTheirOwnOrder)
{
	for (uint32_t i = 0; i < 30; ++i)
	{
		ASSERT_TRUE(sent(*a, *b, i, payload(2000, 1), Lane::Bulk));
		ASSERT_TRUE(sent(*a, *b, 100 + i, payload(16, 1), Lane::Reliable));
	}

	ASSERT_TRUE(waitUntilTrue([this] { return b->events.messageCount() == 60; }, 10s));

	uint32_t nextBulk	  = 0;
	uint32_t nextReliable = 100;

	for (const auto &message : b->events.messages())
	{
		if (message.lane == Lane::Bulk)
			EXPECT_EQ(message.type, nextBulk++);
		else
			EXPECT_EQ(message.type, nextReliable++);
	}
}


TEST_F(NetworkEngineTest, Broadcast_ReachesEveryConnectedPeer)
{
	const auto c = make("pc-c", "10.0.0.3", network->factory("10.0.0.3"));
	ASSERT_TRUE(FakeNet::connect(*a, *c));

	EXPECT_EQ(a->engine.broadcast(9, payload(100, 4), Lane::Reliable), 2u);

	ASSERT_TRUE(waitUntilTrue([&] { return b->events.messageCount() == 1 && c->events.messageCount() == 1; }));
	EXPECT_EQ(b->events.messages()[0].data, payload(100, 4));
	EXPECT_EQ(c->events.messages()[0].type, 9u);
}


TEST_F(NetworkEngineTest, Stats_CountWhatASessionCarried)
{
	EXPECT_FALSE(a->engine.stats(PeerId{12345}).has_value());

	for (uint32_t i = 0; i < 20; ++i)
		ASSERT_TRUE(sent(*a, *b, i, payload(1000, 1)));

	ASSERT_TRUE(a->engine.flush(b->id(), 3s));
	ASSERT_TRUE(waitUntilTrue([this] { return b->engine.stats(a->id()).value_or(PeerStats{}).bytesReceived == 20'000; }));

	const auto stats = a->engine.stats(b->id());
	ASSERT_TRUE(stats.has_value());
	EXPECT_EQ(stats->bytesSent, 20'000u);
	EXPECT_EQ(stats->bytesQueued, 0u);
	EXPECT_GT(stats->rtt.count(), 0);
}


TEST_F(NetworkEngineTest, Flush_WaitsUntilEverythingIsAcknowledged)
{
	for (uint32_t i = 0; i < 20; ++i)
		sent(*a, *b, i, payload(3000, 1));

	EXPECT_TRUE(a->engine.flush(b->id(), 3s));
	EXPECT_TRUE(waitUntilTrue([this] { return b->events.messageCount() == 20; })) << "Acknowledged means received: the event thread hands it to the callback";
}


TEST_F(NetworkEngineTest, Flush_RightAfterSend_SeesTheMessage)
{
	for (uint32_t i = 0; i < 50; ++i)
	{
		ASSERT_TRUE(sent(*a, *b, i, payload(8, 1)));
		ASSERT_TRUE(a->engine.flush(b->id(), 3s));
		ASSERT_TRUE(waitUntilTrue([&] { return b->events.messageCount() == i + 1; }, 1s)) << "Acknowledged means received";
	}

	b->stop(); // nobody acknowledges anymore

	ASSERT_TRUE(sent(*a, *b, 99, payload(8, 1)));
	EXPECT_FALSE(a->engine.flush(b->id(), 20ms)) << "A message that was just accepted counts, also before the I/O thread got to see it";
}


TEST_F(NetworkEngineTest, Flush_ReturnsWhenTheLoopStops)
{
	b->stop(); // nobody acknowledges anymore
	ASSERT_TRUE(sent(*a, *b, 1, payload(4, 0)));

	auto flushed = std::async(std::launch::async, [this] { return a->engine.flush(b->id(), 5s); });
	std::this_thread::sleep_for(50ms);
	a->stop();

	ASSERT_EQ(flushed.wait_for(1s), std::future_status::ready) << "Stopping the I/O loop must wake a waiting flush";
	EXPECT_FALSE(flushed.get()) << "Nothing was acknowledged";
}


TEST_F(NetworkEngineTest, BurstLargerThanTheCongestionWindow_IsDeliveredCompletely)
{
	constexpr uint32_t Messages = 2000; // far more than may be on the wire at once
	for (uint32_t i = 0; i < Messages; ++i)
		ASSERT_TRUE(sent(*a, *b, i, payload(16, 3)));

	ASSERT_TRUE(waitUntilTrue([this] { return b->events.messageCount() == Messages; }, 10s)) << "The backlog is sent as acknowledgements come in";

	const auto received = b->events.messages();
	for (uint32_t i = 0; i < Messages; ++i)
		EXPECT_EQ(received[i].type, i) << "in order at " << i;
}


// ---------------------------------------------------------------------------
// Backpressure
// ---------------------------------------------------------------------------

class SmallQueueNetworkEngineTest : public NetworkEngineTest
{
protected:
	static constexpr size_t QueueCapacity = 8; // messages of 8 bytes

	static EngineConfig		smallQueueConfig()
	{
		EngineConfig config		   = fastConfig();
		config.sendQueueBytes	   = QueueCapacity * 8;
		config.timings.peerTimeout = 30s; // the session must outlive the time B is cut off
		return config;
	}

	void SetUp() override
	{
		start(network->factory(AddressA), FakeNet::CuttableSocket::wrap(network->factory(AddressB), cutB), smallQueueConfig());
		ASSERT_TRUE(FakeNet::connect(*a, *b));
	}

	// Sends until the congestion window and the queue behind it are full. B must be cut off: nothing is acknowledged.
	void fillSendQueue()
	{
		const auto sendUntilRefused = [this]
		{
			while (queued < 10'000 && sent(*a, *b, queued, payload(8, 1)))
				++queued;
		};

		// The I/O thread takes messages out of the queue while its link has room: only after that the queue stays full
		uint32_t before = 0;
		do
		{
			before = queued;
			sendUntilRefused();
			std::this_thread::sleep_for(50ms);
		} while (queued != before);

		ASSERT_GE(queued, QueueCapacity) << "At least the queue itself takes messages";
		ASSERT_LT(queued, 10'000u) << "A full queue refuses a message that may not wait";
		ASSERT_EQ(send(*a, *b, 0, payload(8, 1)), SendResult::QueueFull);
	}

	std::shared_ptr<std::atomic<bool>> cutB = std::make_shared<std::atomic<bool>>(false);
	uint32_t						   queued{0}; // messages A accepted
};


TEST_F(SmallQueueNetworkEngineTest, SendWithTimeout_WaitsForRoomInTheSendQueue)
{
	cutB->store(true);
	fillSendQueue();

	auto waiting = std::async(std::launch::async, [this] { return sent(*a, *b, 9999, payload(8, 1), Lane::Reliable, 5s); });
	EXPECT_EQ(waiting.wait_for(100ms), std::future_status::timeout) << "Without acknowledgements there is no room: the send has to wait";

	cutB->store(false); // acknowledgements flow again

	ASSERT_EQ(waiting.wait_for(3s), std::future_status::ready) << "Room in the queue must wake the waiting send";
	EXPECT_TRUE(waiting.get());

	ASSERT_TRUE(waitUntilTrue([this] { return b->events.messageCount() == queued + 1; }, 10s));
	EXPECT_EQ(b->events.messages().back().type, 9999u) << "The message that waited is delivered last";
}


TEST_F(SmallQueueNetworkEngineTest, SendWithTimeout_GivesUpWhenNoRoomAppears)
{
	cutB->store(true);
	fillSendQueue();

	const auto started = std::chrono::steady_clock::now();
	EXPECT_EQ(send(*a, *b, 1, payload(8, 1), Lane::Reliable, 150ms), SendResult::QueueFull);

	const auto waited = std::chrono::steady_clock::now() - started;
	EXPECT_GE(waited, 150ms) << "The send must use its whole timeout before it gives up";
	EXPECT_LT(waited, 2s);
}


TEST_F(SmallQueueNetworkEngineTest, SendWithTimeout_ReturnsWhenTheLoopStops)
{
	cutB->store(true);
	fillSendQueue();

	auto waiting = std::async(std::launch::async, [this] { return send(*a, *b, 1, payload(8, 1), Lane::Reliable, 10s); });
	std::this_thread::sleep_for(50ms);
	a->stop();

	ASSERT_EQ(waiting.wait_for(2s), std::future_status::ready) << "Stopping the I/O loop must wake a waiting send: no acknowledgement can arrive anymore";
	EXPECT_EQ(waiting.get(), SendResult::NotRunning);
}


TEST_F(SmallQueueNetworkEngineTest, DisconnectWhileSendWaits_LeavesNoLink)
{
	start(FakeNet::SpyDatagramSocket::wrap(network->factory(AddressA), usageA), FakeNet::CuttableSocket::wrap(network->factory(AddressB), cutB), smallQueueConfig());
	ASSERT_TRUE(FakeNet::connect(*a, *b));
	a->engine.setAnnouncing(false);

	cutB->store(true);
	fillSendQueue();

	auto waiting = std::async(std::launch::async, [this] { return send(*a, *b, 9999, payload(8, 1), Lane::Reliable, 10s); });
	std::this_thread::sleep_for(50ms);
	a->engine.disconnect(b->id());

	ASSERT_EQ(waiting.wait_for(2s), std::future_status::ready) << "A session that is being ended must end the wait";
	EXPECT_EQ(waiting.get(), SendResult::NotConnected);

	// What was accepted is still tried for a while, then the goodbye. After that nothing is left to send it.
	ASSERT_TRUE(waitUntilTrue([this] { return endedWith(*a, *b, DisconnectReason::Local); }, 3s));
	std::this_thread::sleep_for(500ms);

	const size_t sends = usageA->sends.load();
	std::this_thread::sleep_for(300ms);

	EXPECT_EQ(usageA->sends.load(), sends) << "Nothing goes to a peer without a session: neither what was queued nor the message that waited";
}


// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------

TEST_F(NetworkEngineTest, Send_NeverTouchesTheSocketOnTheCallersThread)
{
	spyOnA();

	std::thread other(
		[this]
		{
			for (uint32_t i = 0; i < 50; ++i)
				sent(*a, *b, i, payload(2000, 1), Lane::Reliable, 5s);
		});
	const auto otherThread = other.get_id();

	for (uint32_t i = 0; i < 50; ++i)
		sent(*a, *b, 100 + i, payload(8, 1), Lane::Media);

	other.join();
	ASSERT_TRUE(a->engine.flush(b->id(), 5s));

	const auto senders = usageA->sendingThreads();
	ASSERT_EQ(senders.size(), 1u) << "One thread writes to the socket: the I/O thread";
	EXPECT_FALSE(senders.contains(std::this_thread::get_id()));
	EXPECT_FALSE(senders.contains(otherThread));
}


TEST_F(NetworkEngineTest, Callbacks_NeverRunOnTheIoThread)
{
	spyOnA();

	ASSERT_TRUE(sent(*b, *a, 1, payload(8, 1)));
	ASSERT_TRUE(waitUntilTrue([this] { return a->events.messageCount() == 1; }));

	const auto ioThreads = usageA->sendingThreads();
	ASSERT_EQ(ioThreads.size(), 1u);
	EXPECT_FALSE(ioThreads.contains(a->events.messageThread())) << "The application's code must not hold up the I/O loop";
}


TEST_F(NetworkEngineTest, ConcurrentSenders_KeepPerThreadOrder)
{
	constexpr uint32_t		 Threads   = 4;
	constexpr uint32_t		 PerThread = 500;

	std::vector<std::thread> senders;
	for (uint32_t thread = 0; thread < Threads; ++thread)
	{
		senders.emplace_back(
			[this, thread]
			{
				for (uint32_t i = 0; i < PerThread; ++i)
					sent(*a, *b, (thread << 16) | i, payload(16, static_cast<uint8_t>(thread)), Lane::Reliable, 10s);
			});
	}

	for (auto &sender : senders)
		sender.join();

	ASSERT_TRUE(waitUntilTrue([this] { return b->events.messageCount() == Threads * PerThread; }, 20s)) << "Only " << b->events.messageCount() << " arrived";

	std::array<uint32_t, Threads> next{};
	for (const auto &message : b->events.messages())
	{
		const uint32_t thread = message.type >> 16;
		ASSERT_LT(thread, Threads);
		ASSERT_EQ(message.type & 0xFFFF, next[thread]++) << "Order of thread " << thread << " broken";
	}
}


TEST_F(UnconnectedNetworkEngineTest, IdleEngine_OnlyWakesForItsTimers)
{
	EngineConfig config		   = fastConfig();
	config.timings.keepAlive   = 1s;
	config.timings.peerTimeout = 5s;
	start(network->factory(AddressA), network->factory(AddressB), config);
	ASSERT_TRUE(FakeNet::connect(*a, *b));

	ASSERT_TRUE(sent(*a, *b, 1, payload(8, 1)));
	ASSERT_TRUE(a->engine.flush(b->id(), 3s));
	std::this_thread::sleep_for(300ms);

	const auto stepsA = a->engine.loopStats().steps;
	const auto stepsB = b->engine.loopStats().steps;
	std::this_thread::sleep_for(3s);

	// In three seconds: two announcements each, and a question with its answer every second
	EXPECT_LE(a->engine.loopStats().steps - stepsA, 20u) << "Without traffic the I/O thread sleeps until its next timer";
	EXPECT_LE(b->engine.loopStats().steps - stepsB, 20u);
	EXPECT_GE(a->engine.loopStats().steps - stepsA, 3u);
}


TEST_F(NetworkEngineTest, ExceptionOnTheIoThread_EndsEverySessionAndWakesWaiters)
{
	EngineConfig config		   = fastConfig();
	config.timings.peerTimeout = 30s; // the session itself must not give up during the test
	spyOnA(config);

	ASSERT_TRUE(sent(*a, *b, 1, payload(8, 1)));
	ASSERT_TRUE(a->engine.flush(b->id(), 3s));

	b->stop();
	ASSERT_TRUE(sent(*a, *b, 2, payload(8, 1)));
	auto flushed = std::async(std::launch::async, [this] { return a->engine.flush(b->id(), 10s); });
	std::this_thread::sleep_for(50ms);

	usageA->throwOnReceive.store(true);

	ASSERT_TRUE(waitUntilTrue([this] { return !a->events.ended().empty(); })) << "A loop that died must not leave its sessions waiting forever";
	EXPECT_TRUE(endedWith(*a, *b, DisconnectReason::NetworkError));
	EXPECT_EQ(flushed.wait_for(2s), std::future_status::ready) << "Nothing is acknowledged anymore: whoever waits for it must be woken";
	EXPECT_EQ(send(*a, *b, 3, payload(8, 1)), SendResult::NotRunning);
}


TEST_F(NetworkEngineTest, BlockedMessageCallback_DoesNotHoldUpTheIoLoop)
{
	b->events.holdMessages();

	ASSERT_TRUE(sent(*a, *b, 0, payload(8, 1)));
	ASSERT_TRUE(waitUntilTrue([this] { return b->events.enteredMessageCallbacks() == 1; })) << "B's application is now stuck in its callback";

	// B's I/O loop must keep receiving and acknowledging, including a message that spans several send windows
	for (uint32_t i = 1; i <= 100; ++i)
		ASSERT_TRUE(sent(*a, *b, i, payload(8, 1)));

	const auto big = payload(size_t{3} * 1024 * 1024, 5);
	ASSERT_TRUE(sent(*a, *b, 101, big));

	EXPECT_TRUE(a->engine.flush(b->id(), 10s)) << "Everything is acknowledged although no callback returned yet";
	EXPECT_EQ(b->events.messageCount(), 0u);

	b->events.releaseMessages();

	ASSERT_TRUE(waitUntilTrue([this] { return b->events.messageCount() == 102; }, 10s)) << "Nothing that was received while the callback blocked may be lost";

	const auto messages = b->events.messages();
	for (uint32_t i = 0; i < 102; ++i)
		EXPECT_EQ(messages[i].type, i) << "in order at " << i;

	EXPECT_EQ(messages.back().data, big);
}


TEST_F(UnconnectedNetworkEngineTest, SlowApplication_PausesItsSenderInsteadOfPilingUpMessages)
{
	EngineConfig config		   = fastConfig();
	config.timings.peerTimeout = 30s;
	config.maxSendRate		   = 0; // as fast as the test machine goes: only the pause can hold A back

	start(network->factory(AddressA), network->factory(AddressB), config);
	ASSERT_TRUE(FakeNet::connect(*a, *b));

	b->events.holdMessages();

	// Twice what B keeps for an application that does not take its messages
	constexpr size_t   MessageSize = 1024 * 1024;
	constexpr uint32_t Messages	   = 2 * channel::BacklogPauseBytes / MessageSize;

	for (uint32_t i = 0; i < Messages; ++i)
		ASSERT_TRUE(sent(*a, *b, i, payload(MessageSize, static_cast<uint8_t>(i)), Lane::Reliable, 10s));

	EXPECT_FALSE(a->engine.flush(b->id(), 2s)) << "B's application is stuck: A must be held back instead of handing everything over";
	EXPECT_TRUE(a->events.ended().empty()) << "A paused peer is not a lost peer";

	// Sessions are still opened: only the application lanes are paused
	const auto c = make("pc-c", "10.0.0.3", network->factory("10.0.0.3"), config);
	ASSERT_TRUE(waitUntilTrue(
		[&]
		{
			c->engine.setAnnouncing(true);
			b->engine.setAnnouncing(true);
			return c->events.knows(b->id());
		}));
	ASSERT_TRUE(c->engine.connect(b->id()));
	EXPECT_TRUE(waitUntilTrue([&] { return c->events.isConnectedTo(b->id()); })) << "B's I/O loop answers although its application does not";

	// ... and Media is not held back for it
	ASSERT_TRUE(sent(*a, *b, 5000, payload(100, 1), Lane::Media));
	EXPECT_TRUE(waitUntilTrue([&] { return a->engine.stats(b->id()).value_or(PeerStats{}).mediaSent == 1; }));

	b->events.releaseMessages();

	ASSERT_TRUE(waitUntilTrue([this] { return b->events.messageCount() == Messages + 1; }, 30s))
		<< "Only " << b->events.messageCount() << " arrived after the application caught up";

	uint32_t next = 0;
	for (const auto &message : b->events.messages())
	{
		if (message.lane == Lane::Media)
			continue;

		ASSERT_EQ(message.type, next) << "Order broken at " << next;
		ASSERT_EQ(message.data, payload(MessageSize, static_cast<uint8_t>(next)));
		++next;
	}

	EXPECT_TRUE(a->engine.flush(b->id(), 5s));
	EXPECT_TRUE(a->events.ended().empty());
}


TEST_F(NetworkEngineTest, Session_IsOpenedWhileAnotherOneIsSaturated)
{
	// A streams large messages: its windows are full all the time
	std::atomic<bool> stop{false};
	std::thread		  stream(
		[&]
		{
			const auto big = payload(256 * 1024, 1);
			while (!stop.load())
				sent(*a, *b, 1, big, Lane::Reliable, 100ms);
		});

	ASSERT_TRUE(waitUntilTrue([this] { return b->events.messageCount() >= 3; }, 10s)) << "The stream is running";

	const auto c = make("pc-c", "10.0.0.3", network->factory("10.0.0.3"));
	ASSERT_TRUE(FakeNet::discover(*a, *c));

	const auto askedAt = std::chrono::steady_clock::now();
	ASSERT_TRUE(a->engine.connect(c->id()));
	ASSERT_TRUE(waitUntilTrue([&] { return a->events.isConnectedTo(c->id()); }, 5s));
	const auto took = std::chrono::steady_clock::now() - askedAt;

	stop.store(true);
	stream.join();

	EXPECT_LT(took, 1s) << "Session messages have a lane of their own and must not wait behind queued application data";
}


// ---------------------------------------------------------------------------
// Loss of the peer
// ---------------------------------------------------------------------------

TEST_F(NetworkEngineTest, UnacknowledgedMessages_ReportThePeerAsLost)
{
	b->stop(); // B vanishes without a word

	ASSERT_TRUE(sent(*a, *b, 1, payload(4, 0)));
	EXPECT_FALSE(a->engine.flush(b->id(), 50ms));

	ASSERT_TRUE(waitUntilTrue([this] { return !a->events.ended().empty(); }, 5s)) << "A peer that does not answer must be reported";
	EXPECT_TRUE(endedWith(*a, *b, DisconnectReason::Lost));
	EXPECT_EQ(send(*a, *b, 2, payload(4, 0)), SendResult::NotConnected);
}


TEST_F(NetworkEngineTest, KeepAlive_HoldsAnIdleSessionAndDetectsSilence)
{
	std::this_thread::sleep_for(1s); // idle for twice the peer timeout
	EXPECT_TRUE(a->events.ended().empty()) << "An idle session stays alive: the peers ask each other";
	EXPECT_TRUE(b->events.ended().empty());

	b->stop();

	ASSERT_TRUE(waitUntilTrue([this] { return !a->events.ended().empty(); }, 3s)) << "A silent peer must be reported";
	EXPECT_TRUE(endedWith(*a, *b, DisconnectReason::Lost));
}


TEST_F(UnconnectedNetworkEngineTest, DiscoveredPeerWithoutASession_IsNotSupervised)
{
	ASSERT_TRUE(FakeNet::discover(*a, *b));
	b->stop();

	std::this_thread::sleep_for(700ms);
	EXPECT_TRUE(a->events.ended().empty()) << "Only sessions end";
	EXPECT_TRUE(a->events.lost().empty()) << "A peer is forgotten when it stopped announcing for a while, not when it is quiet for a moment";
}


// ---------------------------------------------------------------------------
// Unreliable network
// ---------------------------------------------------------------------------

class LossyNetworkEngineTest : public UnconnectedNetworkEngineTest
{
protected:
	void SetUp() override
	{
		FakeNet::LossProfile profileA{0.3, 0.1, 0.2, 17};
		FakeNet::LossProfile profileB{0.3, 0.1, 0.2, 23};

		// At 30% loss per direction a session must not be given up on as quickly as the other tests want it
		EngineConfig		 config = fastConfig();
		config.timings.peerTimeout	= 10s;

		start(FakeNet::LossyDatagramSocket::wrap(network->factory(AddressA), profileA), FakeNet::LossyDatagramSocket::wrap(network->factory(AddressB), profileB), config);
		ASSERT_TRUE(FakeNet::connect(*a, *b, 10s)) << "Announcements and the handshake get through as well";
	}
};


TEST_F(LossyNetworkEngineTest, EveryMessageArrivesExactlyOnceAndInOrder)
{
	const uint32_t total = 1000;

	for (uint32_t i = 0; i < total; ++i)
	{
		// Every 200th message is large enough to be fragmented into ~90 packets
		const size_t size = i % 200 == 0 ? 100 * 1024 : 16;
		ASSERT_TRUE(sent(*a, *b, i, payload(size, static_cast<uint8_t>(i))));
	}

	ASSERT_TRUE(waitUntilTrue([this, total] { return b->events.messageCount() >= total; }, 30s)) << "Only " << b->events.messageCount() << " of " << total << " arrived";

	std::this_thread::sleep_for(200ms); // duplicates would show up now
	const auto messages = b->events.messages();
	ASSERT_EQ(messages.size(), total) << "No message may be delivered twice";

	for (uint32_t i = 0; i < total; ++i)
	{
		ASSERT_EQ(messages[i].type, i) << "Order broken at " << i;
		ASSERT_EQ(messages[i].data, payload(i % 200 == 0 ? 100 * 1024 : 16, static_cast<uint8_t>(i)));
	}

	EXPECT_TRUE(a->events.ended().empty());
}


TEST_F(LossyNetworkEngineTest, SessionAndMessagesInBothDirections)
{
	for (uint32_t i = 0; i < 100; ++i)
	{
		sent(*a, *b, i, payload(32, 1));
		sent(*b, *a, i, payload(32, 2));
	}

	EXPECT_TRUE(waitUntilTrue([this] { return a->events.messageCount() == 100 && b->events.messageCount() == 100; }, 10s));

	a->engine.disconnect(b->id());
	EXPECT_TRUE(waitUntilTrue([this] { return endedWith(*a, *b, DisconnectReason::Local); }, 10s));
}

} // namespace EngineTests
