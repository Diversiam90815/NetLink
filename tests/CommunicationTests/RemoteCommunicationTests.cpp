#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "Messaging/RemoteCommunication.h"
#include "Transport/TransportInterfaces.h"

using ::testing::_;
using ::testing::Invoke;
using ::testing::Return;


namespace CommunicationTests
{


class MockSession : public netlink::ISession
{
public:
	MOCK_METHOD(bool, isConnected, (), (const, override));
	MOCK_METHOD(bool, sendMessage, (const netlink::InternalMessage &, netlink::DeliveryMode), (override));
	MOCK_METHOD(void, startReadAsync, (MessageReceivedCallback, DisconnectedCallback), (override));
	MOCK_METHOD(void, stopReadAsync, (), (override));
	MOCK_METHOD(int, getBoundPort, (), (const, override));
	MOCK_METHOD(netlink::net::IPv4Address, getRemoteAddress, (), (const, override));
	MOCK_METHOD(int, getRemotePort, (), (const, override));
	MOCK_METHOD(void, close, (), (override));
};


static std::shared_ptr<MockSession> makeMockSession()
{
	return std::make_shared<MockSession>();
}


TEST(RemoteCommunication, NotInitializedByDefault)
{
	RemoteCommunication rc;
	EXPECT_FALSE(rc.isInitialized()) << "A freshly constructed RemoteCommunication must not be initialized until init() is called";
}


TEST(RemoteCommunication, InitWithValidSessionSetsInitialized)
{
	RemoteCommunication rc;
	auto				session = makeMockSession();
	// init() may query isConnected internally — allow any number of calls
	EXPECT_CALL(*session, isConnected()).WillRepeatedly(Return(true));

	bool ok = rc.init(session);
	EXPECT_TRUE(ok) << "init() must return true when given a valid session for the first time";
	EXPECT_TRUE(rc.isInitialized()) << "isInitialized() must return true after a successful init() call";

	rc.deinit();
}


TEST(RemoteCommunication, DeinitClearsInitialized)
{
	RemoteCommunication rc;
	auto				session = makeMockSession();
	EXPECT_CALL(*session, isConnected()).WillRepeatedly(Return(true));

	rc.init(session);
	rc.deinit();

	EXPECT_FALSE(rc.isInitialized()) << "isInitialized() must return false after deinit() tears down the session";
}


TEST(RemoteCommunication, InitTwiceReturnsFalseSecondTime)
{
	RemoteCommunication rc;
	auto				session = makeMockSession();
	EXPECT_CALL(*session, isConnected()).WillRepeatedly(Return(true));

	EXPECT_TRUE(rc.init(session)) << "The first init() call must succeed and return true";
	EXPECT_FALSE(rc.init(session)) << "A second init() call while already initialized must return false — double-init is a no-op";
	rc.deinit();
}


TEST(RemoteCommunication, WriteDoesNotCrashBeforeStart)
{
	RemoteCommunication rc;
	auto				session = makeMockSession();
	EXPECT_CALL(*session, isConnected()).WillRepeatedly(Return(true));

	rc.init(session);
	// write() before start() should queue the message without crashing
	std::vector<uint8_t> data{0x01, 0x02, 0x03};
	EXPECT_NO_THROW(rc.write(1, data)) << "write() must not crash when called after init() but before start() — the message must be queued";
	rc.deinit();
}


TEST(RemoteCommunication, SetMessageCallbackAccepted)
{
	RemoteCommunication rc;
	EXPECT_NO_THROW(rc.setMessageCallback([](uint32_t, std::vector<uint8_t> &) {})) << "setMessageCallback() must accept any valid callable without throwing";
}

TEST(RemoteCommunication, FailedSendRequeuesTheUnsentTail)
{
	// sendMessages() swaps the whole outgoing queue out before sending. A failure part
	// way through used to destroy every message from that point on, without a trace.
	RemoteCommunication	  rc;
	auto				  session = makeMockSession();

	std::vector<uint32_t> sent;
	bool				  failOnSecond = true;

	EXPECT_CALL(*session, isConnected()).WillRepeatedly(Return(true));
	EXPECT_CALL(*session, stopReadAsync()).Times(::testing::AnyNumber());
	EXPECT_CALL(*session, sendMessage(_, _))
		.WillRepeatedly(Invoke(
			[&](const netlink::InternalMessage &message, netlink::DeliveryMode)
			{
				if (failOnSecond && message.type == 2)
					return false;

				sent.push_back(message.type);
				return true;
			}));

	ASSERT_TRUE(rc.init(session));

	// Threads are created but not started, so sendMessages() is driven directly here
	rc.write(1, {0x01});
	rc.write(2, {0x02});
	rc.write(3, {0x03});

	EXPECT_FALSE(rc.sendMessages()) << "A failing send must be reported to the caller";
	EXPECT_EQ(sent, std::vector<uint32_t>{1}) << "Only the first message can have gone out";

	failOnSecond = false;
	EXPECT_TRUE(rc.sendMessages());

	EXPECT_EQ(sent, (std::vector<uint32_t>{1, 2, 3})) << "Messages 2 and 3 must have been requeued in order rather than dropped";

	rc.deinit();
}

} // namespace CommunicationTests
