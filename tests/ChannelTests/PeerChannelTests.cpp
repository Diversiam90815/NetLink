#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "TestIp.h"
#include "Channel/PeerChannel.h"
#include "FakeDatagramNetwork.h"
#include "LossyDatagramSocket.h"
#include "TaskQueue.h"

using namespace netlink;
using namespace std::chrono_literals;


namespace ChannelTests
{

template <typename Predicate>
bool waitUntilTrue(Predicate predicate, std::chrono::milliseconds timeout = 3s)
{
	auto deadline = std::chrono::steady_clock::now() + timeout;
	while (std::chrono::steady_clock::now() < deadline)
	{
		if (predicate())
			return true;
		std::this_thread::sleep_for(5ms);
	}
	return predicate();
}


// Short timers so loss detection runs in test time
static PeerChannelConfig fastConfig()
{
	PeerChannelConfig config;
	config.reliability.initialRto	  = 50ms;
	config.reliability.minRto		  = 10ms;
	config.reliability.maxRto		  = 100ms;
	config.reliability.failureTimeout = 500ms;
	config.heartbeat.interval		  = 50ms;
	config.heartbeat.silenceTimeout	  = 400ms;
	return config;
}


struct ReceivedMessage
{
	std::string			 sender;
	uint32_t			 type{0};
	std::vector<uint8_t> data;
};


// Records what a channel delivers
class Recorder
{
public:
	void attach(PeerChannel &channel)
	{
		ChannelConnectionCallbacks connection;
		connection.onConnectRequested = [this](const std::string &name)
		{
			std::lock_guard<std::mutex> lock(mutex);
			lastSender = name;
			++connectRequests;
		};
		connection.onConnectRequestAnswered = [this](const std::string &, bool accepted, const std::string &reason)
		{
			std::lock_guard<std::mutex> lock(mutex);
			lastAnswer = accepted;
			lastReason = reason;
			++answers;
		};
		connection.onDisconnectReceived = [this](const std::string &) { ++disconnects; };
		connection.onReadyFlagReceived	= [this](const std::string &) { ++readyFlags; };
		channel.setConnectionCallbacks(connection);

		ChannelValidationCallbacks validation;
		validation.onValidationRequestReceived = [this](const std::string &, RemoteRequest request) { lastRequest = static_cast<int>(request); };
		validation.onSecretResponseReceived	   = [this](const std::string &, const std::string &secret)
		{
			std::lock_guard<std::mutex> lock(mutex);
			lastSecret = secret;
		};
		validation.onVersionResponseReceived	 = [this](const std::string &, const std::string &) { ++versions; };
		validation.onValidationHandshakeReceived = [this](const std::string &) { ++handshakes; };
		channel.setValidationCallbacks(validation);

		channel.setMessageCallback(
			[this](const std::string &sender, uint32_t type, std::vector<uint8_t> data)
			{
				std::unique_lock<std::mutex> lock(mutex);

				callbackThread = std::this_thread::get_id();
				++enteredCallbacks;
				released.wait(lock, [this] { return !holding; });

				messages.push_back({sender, type, std::move(data)});
			});

		channel.setOnPeerLost(
			[this](const std::string &name, const std::string &)
			{
				std::lock_guard<std::mutex> lock(mutex);
				lostPeers.push_back(name);
			});
	}

	// Message callbacks block until releaseMessages(), like an application that is busy in its handler
	void holdMessages()
	{
		std::lock_guard<std::mutex> lock(mutex);
		holding = true;
	}

	void releaseMessages()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			holding = false;
		}
		released.notify_all();
	}

	int enteredMessageCallbacks()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return enteredCallbacks;
	}

	std::thread::id messageThread()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return callbackThread;
	}

	size_t messageCount()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return messages.size();
	}

	std::vector<ReceivedMessage> receivedMessages()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return messages;
	}

	std::vector<std::string> lost()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return lostPeers;
	}

	std::string sender()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return lastSender;
	}

	std::string secret()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return lastSecret;
	}

	std::string reason()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return lastReason;
	}

	std::atomic<int>  connectRequests{0};
	std::atomic<int>  answers{0};
	std::atomic<bool> lastAnswer{false};
	std::atomic<int>  disconnects{0};
	std::atomic<int>  readyFlags{0};
	std::atomic<int>  lastRequest{-1};
	std::atomic<int>  versions{0};
	std::atomic<int>  handshakes{0};

private:
	std::mutex					 mutex;
	std::condition_variable		 released;
	bool						 holding{false};
	int							 enteredCallbacks{0};
	std::thread::id				 callbackThread;
	std::string					 lastSender;
	std::string					 lastSecret;
	std::string					 lastReason;
	std::vector<ReceivedMessage> messages;
	std::vector<std::string>	 lostPeers;
};


class PeerChannelTest : public ::testing::Test
{
protected:
	void SetUp() override { connect(network->factory("10.0.0.1"), network->factory("10.0.0.2")); }

	void TearDown() override
	{
		pcA->deinit();
		pcB->deinit();
	}

	void connect(net::DatagramSocketFactory factoryA, net::DatagramSocketFactory factoryB, PeerChannelConfig config = fastConfig())
	{
		pcA = std::make_unique<PeerChannel>(std::move(factoryA), config);
		pcB = std::make_unique<PeerChannel>(std::move(factoryB), config);

		ASSERT_TRUE(pcA->init("pc-a"));
		ASSERT_TRUE(pcB->init("pc-b"));

		pcA->setLocalIPv4(ipv4("10.0.0.1"));
		pcB->setLocalIPv4(ipv4("10.0.0.2"));
		ASSERT_NE(pcA->getBoundPort(), 0);
		ASSERT_NE(pcB->getBoundPort(), 0);

		pcA->registerPeer("pc-b", ipv4("10.0.0.2"), pcB->getBoundPort());
		pcB->registerPeer("pc-a", ipv4("10.0.0.1"), pcA->getBoundPort());

		atA.attach(*pcA);
		atB.attach(*pcB);

		pcA->start();
		pcB->start();
	}

	static std::vector<uint8_t> payload(size_t size, uint8_t seed)
	{
		std::vector<uint8_t> data(size);
		for (size_t i = 0; i < size; ++i)
			data[i] = static_cast<uint8_t>(seed + i * 7);
		return data;
	}

	std::shared_ptr<FakeNet::FakeDatagramNetwork> network = FakeNet::FakeDatagramNetwork::create();

	Recorder									  atA;
	Recorder									  atB;
	std::unique_ptr<PeerChannel>				  pcA;
	std::unique_ptr<PeerChannel>				  pcB;
};


// ---------------------------------------------------------------------------
// Control signals
// ---------------------------------------------------------------------------

TEST_F(PeerChannelTest, ConnectRequest_IsRoutedWithSenderName)
{
	EXPECT_TRUE(pcA->sendConnectRequest("pc-b"));

	ASSERT_TRUE(waitUntilTrue([this] { return atB.connectRequests.load() == 1; }));
	EXPECT_EQ(atB.sender(), "pc-a");
}


TEST_F(PeerChannelTest, PayloadsSurviveTheRoundTrip)
{
	pcA->sendConnectAnswer("pc-b", false, "busy");
	pcA->sendSecretResponse("pc-b", "top-secret");

	ASSERT_TRUE(waitUntilTrue([this] { return atB.answers.load() == 1 && atB.secret() == "top-secret"; }));
	EXPECT_FALSE(atB.lastAnswer.load());
	EXPECT_EQ(atB.reason(), "busy");
}


TEST_F(PeerChannelTest, EverySignal_ReachesItsCallback)
{
	pcA->sendConnectRequest("pc-b");
	pcA->sendConnectAnswer("pc-b", true);
	pcA->sendDisconnect("pc-b");
	pcA->sendReadyFlag("pc-b");
	pcA->sendValidationRequest("pc-b", RemoteRequest::Secret);
	pcA->sendSecretResponse("pc-b", "s");
	pcA->sendVersionResponse("pc-b", "1.0");
	pcA->sendValidationHandshake("pc-b");

	EXPECT_TRUE(waitUntilTrue(
		[this]
		{
			return atB.connectRequests.load() == 1 && atB.answers.load() == 1 && atB.disconnects.load() == 1 && atB.readyFlags.load() == 1 &&
				   atB.lastRequest.load() == static_cast<int>(RemoteRequest::Secret) && atB.secret() == "s" && atB.versions.load() == 1 && atB.handshakes.load() == 1;
		}))
		<< "Each signal type must round-trip through serialization and routing";
}


TEST_F(PeerChannelTest, SendToUnknownPeer_Fails)
{
	EXPECT_FALSE(pcA->sendConnectRequest("pc-unknown"));
	EXPECT_FALSE(pcA->sendMessage("pc-unknown", 1, payload(4, 0), DeliveryMode::ReliableOrdered));
}


TEST_F(PeerChannelTest, SendBeforeBinding_Fails)
{
	PeerChannel unbound(network->factory("10.0.0.3"));
	ASSERT_TRUE(unbound.init("pc-c"));
	unbound.registerPeer("pc-b", ipv4("10.0.0.2"), pcB->getBoundPort());

	EXPECT_FALSE(unbound.sendConnectRequest("pc-b")) << "Without a socket nothing can be queued";
}


TEST_F(PeerChannelTest, Rebind_WhileRunning_KeepsReceiving)
{
	const int oldPort	= pcB->getBoundPort();
	int		  boundPort = 0;
	pcB->setOnSocketBound([&](int port) { boundPort = port; });

	pcB->setLocalIPv4(ipv4("10.0.0.2")); // e.g. adapter change

	ASSERT_NE(pcB->getBoundPort(), oldPort) << "Rebinding picks a new OS assigned port";
	EXPECT_EQ(boundPort, pcB->getBoundPort()) << "The new port must be reported so discovery can announce it";

	pcA->registerPeer("pc-b", ipv4("10.0.0.2"), pcB->getBoundPort());
	pcA->sendConnectRequest("pc-b");

	EXPECT_TRUE(waitUntilTrue([this] { return atB.connectRequests.load() == 1; })) << "The I/O loop must pick up the new socket without a restart";
}


TEST_F(PeerChannelTest, Deinit_IsIdempotent)
{
	pcA->deinit();
	EXPECT_NO_THROW(pcA->deinit());
	EXPECT_EQ(pcA->getBoundPort(), 0);
}


TEST_F(PeerChannelTest, ForeignDatagrams_AreIgnored)
{
	auto intruder = network->factory("10.0.0.9")({ipv4("10.0.0.9"), 0}, {});
	ASSERT_TRUE(intruder.has_value());

	const std::string oldProtocol = R"({"type":0,"name":"pc-x","IPv4":"10.0.0.9","sigPort":1})";
	static_cast<void>(
		(*intruder)->sendTo({ipv4("10.0.0.2"), static_cast<uint16_t>(pcB->getBoundPort())}, std::span(reinterpret_cast<const uint8_t *>(oldProtocol.data()), oldProtocol.size())));

	const std::vector<uint8_t> junk{0x4E, 0x4C, 0x01};
	static_cast<void>((*intruder)->sendTo({ipv4("10.0.0.2"), static_cast<uint16_t>(pcB->getBoundPort())}, junk));

	pcA->sendConnectRequest("pc-b");

	ASSERT_TRUE(waitUntilTrue([this] { return atB.connectRequests.load() == 1; }));
	EXPECT_EQ(atB.sender(), "pc-a") << "Datagrams of an older build or garbage must neither crash nor be routed";
}


// ---------------------------------------------------------------------------
// Application messages
// ---------------------------------------------------------------------------

TEST_F(PeerChannelTest, ApplicationMessages_ArriveInOrderWithTypeAndSender)
{
	for (uint32_t i = 0; i < 50; ++i)
		ASSERT_TRUE(pcA->sendMessage("pc-b", i, payload(i, static_cast<uint8_t>(i)), DeliveryMode::ReliableOrdered));

	ASSERT_TRUE(waitUntilTrue([this] { return atB.messageCount() == 50; }));

	const auto messages = atB.receivedMessages();
	for (uint32_t i = 0; i < 50; ++i)
	{
		EXPECT_EQ(messages[i].sender, "pc-a");
		EXPECT_EQ(messages[i].type, i);
		EXPECT_EQ(messages[i].data, payload(i, static_cast<uint8_t>(i)));
	}
}


TEST_F(PeerChannelTest, SendingInBothDirectionsAtOnce_KeepsTheOrder)
{
	// Each channel sends from its own thread while its I/O thread receives, acknowledges and sends as well
	constexpr uint32_t Messages = 3000;

	const auto		   sendAll	= [](PeerChannel &channel, const char *to)
	{
		for (uint32_t i = 0; i < Messages; ++i)
		{
			if (!channel.sendMessage(to, i, payload(64, static_cast<uint8_t>(i)), DeliveryMode::ReliableOrdered, 10s))
				return false;
		}
		return true;
	};

	auto fromA = std::async(std::launch::async, [&] { return sendAll(*pcA, "pc-b"); });
	auto fromB = std::async(std::launch::async, [&] { return sendAll(*pcB, "pc-a"); });

	ASSERT_TRUE(fromA.get());
	ASSERT_TRUE(fromB.get());
	ASSERT_TRUE(waitUntilTrue([this] { return atA.messageCount() == Messages && atB.messageCount() == Messages; }, 20s))
		<< atA.messageCount() << " at A, " << atB.messageCount() << " at B";

	for (auto *recorder : {&atA, &atB})
	{
		const auto messages = recorder->receivedMessages();
		for (uint32_t i = 0; i < Messages; ++i)
			ASSERT_EQ(messages[i].type, i) << "Order broken at " << i;
	}
}


TEST_F(PeerChannelTest, LargeMessage_IsFragmentedAndReassembled)
{
	const auto big = payload(size_t{512} * 1024, 3);

	ASSERT_TRUE(pcA->sendMessage("pc-b", 77, big, DeliveryMode::ReliableOrdered));
	ASSERT_TRUE(waitUntilTrue([this] { return atB.messageCount() == 1; }, 10s));

	const auto messages = atB.receivedMessages();
	EXPECT_EQ(messages[0].type, 77u);
	EXPECT_EQ(messages[0].data, big);
}


TEST_F(PeerChannelTest, UnreliableMessages_AreDelivered)
{
	for (uint32_t i = 0; i < 10; ++i)
		ASSERT_TRUE(pcA->sendMessage("pc-b", i, payload(8, 0), DeliveryMode::UnreliableSequenced));

	EXPECT_TRUE(waitUntilTrue([this] { return atB.messageCount() == 10; })) << "Without loss every unreliable message arrives";
	EXPECT_FALSE(pcA->sendMessage("pc-b", 1, payload(2000, 0), DeliveryMode::UnreliableSequenced)) << "An unreliable message must fit into one datagram";
}


TEST_F(PeerChannelTest, ApplicationMessageFromUnregisteredRemote_IsDropped)
{
	PeerChannel stranger(network->factory("10.0.0.3"), fastConfig());
	ASSERT_TRUE(stranger.init("pc-c"));
	stranger.setLocalIPv4(ipv4("10.0.0.3"));
	stranger.registerPeer("pc-b", ipv4("10.0.0.2"), pcB->getBoundPort());
	stranger.start();

	ASSERT_TRUE(stranger.sendMessage("pc-b", 1, payload(4, 0), DeliveryMode::ReliableOrdered));
	ASSERT_TRUE(stranger.flush("pc-b", 2s)) << "The message is still acknowledged, so the sender does not retry forever";

	EXPECT_EQ(atB.messageCount(), 0u) << "Only discovered peers may deliver application data";
	stranger.deinit();
}


TEST_F(PeerChannelTest, Flush_WaitsUntilEverythingIsAcknowledged)
{
	for (uint32_t i = 0; i < 20; ++i)
		pcA->sendMessage("pc-b", i, payload(3000, 1), DeliveryMode::ReliableOrdered);

	EXPECT_TRUE(pcA->flush("pc-b", 3s));
	EXPECT_TRUE(waitUntilTrue([this] { return atB.messageCount() == 20; })) << "Acknowledged means received: the delivery thread hands it to the callback";
}


TEST_F(PeerChannelTest, Flush_ReturnsWhenTheLoopStops)
{
	pcB->deinit(); // nobody acknowledges anymore
	ASSERT_TRUE(pcA->sendMessage("pc-b", 1, payload(4, 0), DeliveryMode::ReliableOrdered));

	auto flushed = std::async(std::launch::async, [this] { return pcA->flush("pc-b", 5s); });
	std::this_thread::sleep_for(50ms);
	pcA->stop();

	ASSERT_EQ(flushed.wait_for(1s), std::future_status::ready) << "Stopping the I/O loop must wake a waiting flush";
	EXPECT_FALSE(flushed.get()) << "Nothing was acknowledged";
}


TEST_F(PeerChannelTest, BurstLargerThanTheCongestionWindow_IsDeliveredCompletely)
{
	constexpr uint32_t Messages = 2000; // far more than may be on the wire at once
	for (uint32_t i = 0; i < Messages; ++i)
		ASSERT_TRUE(pcA->sendMessage("pc-b", i, payload(16, 3), DeliveryMode::ReliableOrdered));

	ASSERT_TRUE(waitUntilTrue([this] { return atB.messageCount() == Messages; }, 10s)) << "The backlog is sent as acknowledgements come in";

	const auto received = atB.receivedMessages();
	for (uint32_t i = 0; i < Messages; ++i)
		EXPECT_EQ(received[i].type, i) << "in order at " << i;
}


// ---------------------------------------------------------------------------
// Backpressure
// ---------------------------------------------------------------------------

class SmallQueuePeerChannelTest : public PeerChannelTest
{
protected:
	static constexpr size_t QueueCapacity = 8;

	void					SetUp() override
	{
		PeerChannelConfig config			 = fastConfig();
		config.reliability.sendQueueCapacity = QueueCapacity;
		config.reliability.sendQueueOverflow = OverflowPolicy::DropNewest;
		config.reliability.failureTimeout	 = 30s; // the link must outlive the time B is stopped
		connect(network->factory("10.0.0.1"), network->factory("10.0.0.2"), config);
	}

	// Sends until the congestion window and the queue behind it are full. B must be stopped: nothing is acknowledged.
	void fillSendQueue()
	{
		while (queued < 10'000 && pcA->sendMessage("pc-b", queued, payload(8, 1), DeliveryMode::ReliableOrdered))
			++queued;

		ASSERT_GE(queued, QueueCapacity) << "At least the queue itself takes messages";
		ASSERT_LT(queued, 10'000u) << "A full queue refuses a message that may not wait";
	}

	uint32_t queued{0}; // messages A accepted
};


TEST_F(SmallQueuePeerChannelTest, SendWithTimeout_WaitsForRoomInTheSendQueue)
{
	pcB->stop();
	fillSendQueue();

	auto sent = std::async(std::launch::async, [this] { return pcA->sendMessage("pc-b", 9999, payload(8, 1), DeliveryMode::ReliableOrdered, 5s); });
	EXPECT_EQ(sent.wait_for(100ms), std::future_status::timeout) << "Without acknowledgements there is no room: the send has to wait";

	pcB->start(); // acknowledgements flow again

	ASSERT_EQ(sent.wait_for(3s), std::future_status::ready) << "Room in the queue must wake the waiting send";
	EXPECT_TRUE(sent.get());

	ASSERT_TRUE(waitUntilTrue([this] { return atB.messageCount() == queued + 1; }, 10s));
	EXPECT_EQ(atB.receivedMessages().back().type, 9999u) << "The message that waited is delivered last";
}


TEST_F(SmallQueuePeerChannelTest, SendWithTimeout_GivesUpWhenNoRoomAppears)
{
	pcB->stop();
	fillSendQueue();

	const auto started = std::chrono::steady_clock::now();
	EXPECT_FALSE(pcA->sendMessage("pc-b", 1, payload(8, 1), DeliveryMode::ReliableOrdered, 150ms));

	const auto waited = std::chrono::steady_clock::now() - started;
	EXPECT_GE(waited, 150ms) << "The send must use its whole timeout before it gives up";
	EXPECT_LT(waited, 2s);
}


TEST_F(SmallQueuePeerChannelTest, SendWithTimeout_ReturnsWhenTheLoopStops)
{
	pcB->stop();
	fillSendQueue();

	auto sent = std::async(std::launch::async, [this] { return pcA->sendMessage("pc-b", 1, payload(8, 1), DeliveryMode::ReliableOrdered, 10s); });
	std::this_thread::sleep_for(50ms);
	pcA->stop();

	ASSERT_EQ(sent.wait_for(2s), std::future_status::ready) << "Stopping the I/O loop must wake a waiting send: no acknowledgement can arrive anymore";
	EXPECT_FALSE(sent.get());
}


// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------

TEST_F(PeerChannelTest, BlockedMessageCallback_DoesNotHoldUpTheIoLoop)
{
	atB.holdMessages();

	ASSERT_TRUE(pcA->sendMessage("pc-b", 0, payload(8, 1), DeliveryMode::ReliableOrdered));
	ASSERT_TRUE(waitUntilTrue([this] { return atB.enteredMessageCallbacks() == 1; })) << "B's application is now stuck in its callback";

	// B's I/O loop must keep receiving and acknowledging, including a message that spans several send windows
	for (uint32_t i = 1; i <= 100; ++i)
		ASSERT_TRUE(pcA->sendMessage("pc-b", i, payload(8, 1), DeliveryMode::ReliableOrdered));

	const auto big = payload(size_t{3} * 1024 * 1024, 5);
	ASSERT_TRUE(pcA->sendMessage("pc-b", 101, big, DeliveryMode::ReliableOrdered));

	EXPECT_TRUE(pcA->flush("pc-b", 10s)) << "Everything is acknowledged although no callback returned yet";
	EXPECT_EQ(atB.messageCount(), 0u);

	atB.releaseMessages();

	ASSERT_TRUE(waitUntilTrue([this] { return atB.messageCount() == 102; }, 10s)) << "Nothing that was received while the callback blocked may be lost";

	const auto messages = atB.receivedMessages();
	for (uint32_t i = 0; i < 102; ++i)
		EXPECT_EQ(messages[i].type, i) << "in order at " << i;

	EXPECT_EQ(messages.back().data, big);
}


TEST_F(PeerChannelTest, SlowApplication_PausesItsSenderInsteadOfPilingUpMessages)
{
	// B may hold 64 KiB of undelivered payload before it asks its senders to pause
	PeerChannelConfig config	= fastConfig();
	config.deliveryBacklogLimit = 64 * 1024;
	config.reliability.failureTimeout = 30s;

	pcA->deinit();
	pcB->deinit();
	connect(network->factory("10.0.0.1"), network->factory("10.0.0.2"), config);

	atB.holdMessages();

	constexpr uint32_t Messages = 300;
	for (uint32_t i = 0; i < Messages; ++i)
		ASSERT_TRUE(pcA->sendMessage("pc-b", i, payload(8 * 1024, static_cast<uint8_t>(i)), DeliveryMode::ReliableOrdered));

	// 2.4 MiB are waiting at A. B took what was on the way when it noticed, and from then on only a trickle.
	EXPECT_FALSE(pcA->flush("pc-b", 500ms)) << "B's application is stuck: A must be held back instead of handing everything over";
	EXPECT_TRUE(atA.lost().empty()) << "A paused peer is not a lost peer";

	// Control signals still get through
	ASSERT_TRUE(pcA->sendConnectRequest("pc-b"));
	ASSERT_TRUE(pcB->sendReadyFlag("pc-a"));
	EXPECT_TRUE(waitUntilTrue([this] { return atA.readyFlags.load() == 1; })) << "Only the application channel is paused";

	atB.releaseMessages();

	ASSERT_TRUE(waitUntilTrue([this] { return atB.messageCount() == Messages; }, 20s)) << "Only " << atB.messageCount() << " arrived after the application caught up";

	const auto messages = atB.receivedMessages();
	for (uint32_t i = 0; i < Messages; ++i)
	{
		ASSERT_EQ(messages[i].type, i) << "Order broken at " << i;
		ASSERT_EQ(messages[i].data, payload(8 * 1024, static_cast<uint8_t>(i)));
	}

	EXPECT_TRUE(pcA->flush("pc-b", 5s));
	EXPECT_TRUE(atA.lost().empty());
}


TEST_F(PeerChannelTest, ControlSignal_ArrivesWhileTheApplicationChannelIsSaturated)
{
	// A streams large messages: its application window is full all the time
	std::atomic<bool> stop{false};
	std::thread		  stream(
		  [&]
		  {
			  const auto big = payload(256 * 1024, 1);
			  while (!stop.load())
				  pcA->sendMessage("pc-b", 1, big, DeliveryMode::ReliableOrdered, 100ms);
		  });

	ASSERT_TRUE(waitUntilTrue([this] { return atB.messageCount() >= 3; }, 10s)) << "The stream is running";

	const auto sentAt = std::chrono::steady_clock::now();
	ASSERT_TRUE(pcA->sendConnectRequest("pc-b"));
	ASSERT_TRUE(waitUntilTrue([this] { return atB.connectRequests.load() == 1; }, 5s));
	const auto took = std::chrono::steady_clock::now() - sentAt;

	stop.store(true);
	stream.join();

	EXPECT_LT(took, 1s) << "A control signal has a stream of its own and must not wait behind queued application data";
}


TEST_F(PeerChannelTest, ApplicationQueue_ReceivesTheMessages_AndKeepsControlSignalsFlowing)
{
	TaskQueue applicationThread;
	applicationThread.start();

	std::thread::id applicationThreadId;
	std::promise<void> known;
	applicationThread.post(
		[&]
		{
			applicationThreadId = std::this_thread::get_id();
			known.set_value();
		});
	known.get_future().wait();

	Recorder	atC;
	PeerChannel pcC(network->factory("10.0.0.3"), fastConfig(), &applicationThread);
	ASSERT_TRUE(pcC.init("pc-c"));
	pcC.setLocalIPv4(ipv4("10.0.0.3"));
	pcC.registerPeer("pc-a", ipv4("10.0.0.1"), pcA->getBoundPort());
	pcA->registerPeer("pc-c", ipv4("10.0.0.3"), pcC.getBoundPort());
	atC.attach(pcC);
	pcC.start();

	atC.holdMessages();

	ASSERT_TRUE(pcA->sendMessage("pc-c", 1, payload(8, 1), DeliveryMode::ReliableOrdered));
	ASSERT_TRUE(waitUntilTrue([&] { return atC.enteredMessageCallbacks() == 1; }));
	EXPECT_EQ(atC.messageThread(), applicationThreadId) << "Application messages must be delivered on the queue the channel was given";

	// The application is busy: connection signals have their own thread and still get through
	ASSERT_TRUE(pcA->sendConnectRequest("pc-c"));
	EXPECT_TRUE(waitUntilTrue([&] { return atC.connectRequests.load() == 1; })) << "A slow application must not delay control signals";

	atC.releaseMessages();
	EXPECT_TRUE(waitUntilTrue([&] { return atC.messageCount() == 1; }));

	pcC.deinit();
	applicationThread.stop();
}


// ---------------------------------------------------------------------------
// Loss of the peer
// ---------------------------------------------------------------------------

TEST_F(PeerChannelTest, UnacknowledgedMessages_ReportThePeerAsLost)
{
	pcB->deinit(); // B vanishes without a word

	ASSERT_TRUE(pcA->sendMessage("pc-b", 1, payload(4, 0), DeliveryMode::ReliableOrdered));
	EXPECT_FALSE(pcA->flush("pc-b", 50ms));

	ASSERT_TRUE(waitUntilTrue([this] { return !atA.lost().empty(); }, 5s)) << "Exhausted retransmissions must report the peer";
	EXPECT_EQ(atA.lost().front(), "pc-b");
}


TEST_F(PeerChannelTest, KeepAlive_HoldsAnIdleSessionAndDetectsSilence)
{
	pcA->setKeepAlive("pc-b", true);
	pcB->setKeepAlive("pc-a", true);

	std::this_thread::sleep_for(1s); // idle for more than twice the silence timeout
	EXPECT_TRUE(atA.lost().empty()) << "Heartbeats must keep an idle session alive";
	EXPECT_TRUE(atB.lost().empty());

	pcB->deinit();

	ASSERT_TRUE(waitUntilTrue([this] { return !atA.lost().empty(); }, 3s)) << "A silent peer must be reported";
	EXPECT_EQ(atA.lost().front(), "pc-b");
}


TEST_F(PeerChannelTest, WithoutKeepAlive_SilenceIsNotReported)
{
	pcB->deinit();

	std::this_thread::sleep_for(700ms);
	EXPECT_TRUE(atA.lost().empty()) << "Peers outside a session are not supervised";
}


// ---------------------------------------------------------------------------
// Unreliable network
// ---------------------------------------------------------------------------

class LossyPeerChannelTest : public PeerChannelTest
{
protected:
	void SetUp() override
	{
		FakeNet::LossProfile profileA{0.3, 0.1, 0.2, 17};
		FakeNet::LossProfile profileB{0.3, 0.1, 0.2, 23};

		// At 30% loss per direction a link must not be given up on as quickly as the other tests want it
		PeerChannelConfig	 config		  = fastConfig();
		config.reliability.failureTimeout = 10s;
		config.heartbeat.silenceTimeout	  = 5s;

		connect(FakeNet::LossyDatagramSocket::wrap(network->factory("10.0.0.1"), profileA), FakeNet::LossyDatagramSocket::wrap(network->factory("10.0.0.2"), profileB), config);
	}
};


TEST_F(LossyPeerChannelTest, EveryMessageArrivesExactlyOnceAndInOrder)
{
	const uint32_t total = 1000;

	for (uint32_t i = 0; i < total; ++i)
	{
		// Every 200th message is large enough to be fragmented into ~90 packets
		const size_t size = i % 200 == 0 ? 100 * 1024 : 16;
		ASSERT_TRUE(pcA->sendMessage("pc-b", i, payload(size, static_cast<uint8_t>(i)), DeliveryMode::ReliableOrdered));
	}

	ASSERT_TRUE(waitUntilTrue([this, total] { return atB.messageCount() >= total; }, 30s)) << "Only " << atB.messageCount() << " of " << total << " arrived";

	std::this_thread::sleep_for(200ms); // duplicates would show up now
	const auto messages = atB.receivedMessages();
	ASSERT_EQ(messages.size(), total) << "No message may be delivered twice";

	for (uint32_t i = 0; i < total; ++i)
	{
		ASSERT_EQ(messages[i].type, i) << "Order broken at " << i;
		ASSERT_EQ(messages[i].data, payload(i % 200 == 0 ? 100 * 1024 : 16, static_cast<uint8_t>(i)));
	}

	EXPECT_TRUE(atA.lost().empty());
}


TEST_F(LossyPeerChannelTest, SignalsAndMessagesInBothDirections)
{
	pcA->sendConnectRequest("pc-b");
	pcB->sendConnectAnswer("pc-a", true);
	pcB->sendReadyFlag("pc-a");

	for (uint32_t i = 0; i < 100; ++i)
	{
		pcA->sendMessage("pc-b", i, payload(32, 1), DeliveryMode::ReliableOrdered);
		pcB->sendMessage("pc-a", i, payload(32, 2), DeliveryMode::ReliableOrdered);
	}

	EXPECT_TRUE(waitUntilTrue([this] { return atB.connectRequests.load() == 1 && atA.answers.load() == 1 && atA.readyFlags.load() == 1; }, 10s));
	EXPECT_TRUE(waitUntilTrue([this] { return atA.messageCount() == 100 && atB.messageCount() == 100; }, 10s));
}

} // namespace ChannelTests
