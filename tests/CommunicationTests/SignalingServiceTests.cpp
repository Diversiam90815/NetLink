#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

#include "Signaling/SignalingService.h"
#include "FakeDatagramNetwork.h"

using namespace netlink;
using namespace std::chrono_literals;


namespace CommunicationTests
{

template <typename Predicate>
bool waitUntilTrue(Predicate predicate, std::chrono::milliseconds timeout = 2s)
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


class SignalingServiceTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		ASSERT_TRUE(pcA.init("pc-a"));
		ASSERT_TRUE(pcB.init("pc-b"));

		pcA.setLocalIPv4("10.0.0.1");
		pcB.setLocalIPv4("10.0.0.2");
		ASSERT_NE(pcA.getBoundPort(), 0);
		ASSERT_NE(pcB.getBoundPort(), 0);

		pcA.registerPeer("pc-b", "10.0.0.2", pcB.getBoundPort());
		pcB.registerPeer("pc-a", "10.0.0.1", pcA.getBoundPort());

		SignalingCallbacks callbacks;
		callbacks.onConnectRequested = [this](const std::string &name)
		{
			std::lock_guard<std::mutex> lock(mutex);
			lastSender = name;
			++connectRequests;
		};
		callbacks.onConnectRequestAnswered = [this](const std::string &, bool accepted)
		{
			lastAnswer = accepted;
			++answers;
		};
		callbacks.onDataPortReceived = [this](const std::string &, int port) { lastDataPort = port; };
		callbacks.onSecretResponseReceived = [this](const std::string &, const std::string &secret)
		{
			std::lock_guard<std::mutex> lock(mutex);
			lastSecret = secret;
		};
		pcB.setCallbacks(callbacks);

		pcA.start();
		pcB.start();
	}

	std::string sender()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return lastSender;
	}

	std::shared_ptr<FakeNet::FakeDatagramNetwork> network = FakeNet::FakeDatagramNetwork::create();

	std::mutex									  mutex;
	std::string									  lastSender;
	std::string									  lastSecret;
	std::atomic<int>							  connectRequests{0};
	std::atomic<int>							  answers{0};
	std::atomic<bool>							  lastAnswer{false};
	std::atomic<int>							  lastDataPort{0};

	SignalingService							  pcA{network->factory("10.0.0.1")};
	SignalingService							  pcB{network->factory("10.0.0.2")};
};


TEST_F(SignalingServiceTest, ConnectRequest_IsRoutedWithSenderName)
{
	pcA.sendConnectRequest("pc-b");

	ASSERT_TRUE(waitUntilTrue([this] { return connectRequests.load() == 1; }));
	EXPECT_EQ(sender(), "pc-a");
}


TEST_F(SignalingServiceTest, PayloadsSurviveTheRoundTrip)
{
	pcA.sendConnectAnswer("pc-b", true);
	pcA.sendDataPort("pc-b", 43210);
	pcA.sendSecretResponse("pc-b", "top-secret");

	ASSERT_TRUE(waitUntilTrue([this] { return answers.load() == 1 && lastDataPort.load() == 43210; }));
	EXPECT_TRUE(lastAnswer.load());

	ASSERT_TRUE(waitUntilTrue(
		[this]
		{
			std::lock_guard<std::mutex> lock(mutex);
			return lastSecret == "top-secret";
		}));
}


TEST_F(SignalingServiceTest, SendToUnknownPeer_IsDropped)
{
	pcA.sendConnectRequest("pc-unknown");

	std::this_thread::sleep_for(100ms);
	EXPECT_EQ(connectRequests.load(), 0);
}


TEST_F(SignalingServiceTest, Rebind_WhileRunning_KeepsReceiving)
{
	const int oldPort	= pcB.getBoundPort();
	int		  boundPort = 0;
	pcB.setOnSocketBound([&](int port) { boundPort = port; });

	pcB.setLocalIPv4("10.0.0.2"); // e.g. adapter change

	ASSERT_NE(pcB.getBoundPort(), oldPort) << "Rebinding picks a new OS assigned port";
	EXPECT_EQ(boundPort, pcB.getBoundPort()) << "The new port must be reported so discovery can announce it";

	pcA.registerPeer("pc-b", "10.0.0.2", pcB.getBoundPort());
	pcA.sendConnectRequest("pc-b");

	EXPECT_TRUE(waitUntilTrue([this] { return connectRequests.load() == 1; })) << "The receive loop must pick up the new socket without a restart";
}


TEST_F(SignalingServiceTest, Deinit_IsIdempotent)
{
	pcA.deinit();
	EXPECT_NO_THROW(pcA.deinit());
	EXPECT_EQ(pcA.getBoundPort(), 0);
}

} // namespace CommunicationTests
