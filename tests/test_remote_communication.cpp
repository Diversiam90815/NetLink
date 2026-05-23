#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "Messaging/RemoteCommunication.h"
#include "Transport/TransportInterfaces.h"

using ::testing::Return;
using ::testing::_;

// ---------------------------------------------------------------------------
// Mock ISession
// ---------------------------------------------------------------------------

class MockSession : public ISession
{
public:
    MOCK_METHOD(bool, isConnected,    (),                       (const, override));
    MOCK_METHOD(bool, sendMessage,    (netlink::InternalMessage &), (override));
    MOCK_METHOD(void, startReadAsync, (MessageReceivedCallback), (override));
    MOCK_METHOD(void, stopReadAsync,  (),                       (override));
    MOCK_METHOD(int,  getBoundPort,   (),                       (const, override));
};

// ---------------------------------------------------------------------------
// Helper
// ---------------------------------------------------------------------------

static std::shared_ptr<MockSession> makeMockSession()
{
    return std::make_shared<MockSession>();
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(RemoteCommunication, NotInitializedByDefault)
{
    RemoteCommunication rc;
    EXPECT_FALSE(rc.isInitialized());
}

TEST(RemoteCommunication, InitWithValidSessionSetsInitialized)
{
    RemoteCommunication rc;
    auto                session = makeMockSession();
    // init() may query isConnected — allow any call
    EXPECT_CALL(*session, isConnected()).WillRepeatedly(Return(true));

    bool ok = rc.init(session, "secret");
    EXPECT_TRUE(ok);
    EXPECT_TRUE(rc.isInitialized());

    rc.deinit();
}

TEST(RemoteCommunication, DeinitClearsInitialized)
{
    RemoteCommunication rc;
    auto                session = makeMockSession();
    EXPECT_CALL(*session, isConnected()).WillRepeatedly(Return(true));

    rc.init(session, "secret");
    rc.deinit();

    EXPECT_FALSE(rc.isInitialized());
}

TEST(RemoteCommunication, InitTwiceReturnsFalseSecondTime)
{
    RemoteCommunication rc;
    auto                session = makeMockSession();
    EXPECT_CALL(*session, isConnected()).WillRepeatedly(Return(true));

    EXPECT_TRUE(rc.init(session, "secret"));
    EXPECT_FALSE(rc.init(session, "secret")); // already initialized
    rc.deinit();
}

TEST(RemoteCommunication, WriteDoesNotCrashBeforeStart)
{
    RemoteCommunication        rc;
    auto                       session = makeMockSession();
    EXPECT_CALL(*session, isConnected()).WillRepeatedly(Return(true));

    rc.init(session, "secret");
    // write() before start() queues the message — must not crash
    std::vector<uint8_t> data{0x01, 0x02, 0x03};
    EXPECT_NO_THROW(rc.write(1, data));
    rc.deinit();
}

TEST(RemoteCommunication, SetMessageCallbackAccepted)
{
    RemoteCommunication rc;
    EXPECT_NO_THROW(rc.setMessageCallback([](uint32_t, std::vector<uint8_t> &) {}));
}
