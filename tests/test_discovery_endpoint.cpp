#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "DiscoveryEndpoint.h"

// ---------------------------------------------------------------------------
// isValid
// ---------------------------------------------------------------------------

TEST(DiscoveryEndpoint, IsValidWithIPAndPort)
{
    DiscoveryEndpoint ep{"192.168.1.1", 5000, "mypc"};
    EXPECT_TRUE(ep.isValid());
}

TEST(DiscoveryEndpoint, IsValidRequiresNonEmptyIP)
{
    DiscoveryEndpoint ep{"", 5000, "mypc"};
    EXPECT_FALSE(ep.isValid());
}

TEST(DiscoveryEndpoint, IsValidRequiresNonZeroPort)
{
    DiscoveryEndpoint ep{"192.168.1.1", 0, "mypc"};
    EXPECT_FALSE(ep.isValid());
}

// ---------------------------------------------------------------------------
// isEmpty
// ---------------------------------------------------------------------------

TEST(DiscoveryEndpoint, IsEmptyWhenDefaultConstructed)
{
    DiscoveryEndpoint ep{};
    EXPECT_TRUE(ep.isEmpty());
}

TEST(DiscoveryEndpoint, IsNotEmptyWithIPOnly)
{
    DiscoveryEndpoint ep{"10.0.0.1", 0, ""};
    EXPECT_FALSE(ep.isEmpty());
}

TEST(DiscoveryEndpoint, IsNotEmptyWithPortOnly)
{
    DiscoveryEndpoint ep{"", 1234, ""};
    EXPECT_FALSE(ep.isEmpty());
}

// ---------------------------------------------------------------------------
// operator==
// ---------------------------------------------------------------------------

TEST(DiscoveryEndpoint, EqualityMatchesIPAndPort)
{
    DiscoveryEndpoint a{"10.0.0.1", 5000, "alpha"};
    DiscoveryEndpoint b{"10.0.0.1", 5000, "beta"}; // different name — still equal
    EXPECT_EQ(a, b);
}

TEST(DiscoveryEndpoint, InequalityDifferentIP)
{
    DiscoveryEndpoint a{"10.0.0.1", 5000, ""};
    DiscoveryEndpoint b{"10.0.0.2", 5000, ""};
    EXPECT_NE(a, b);
}

TEST(DiscoveryEndpoint, InequalityDifferentPort)
{
    DiscoveryEndpoint a{"10.0.0.1", 5000, ""};
    DiscoveryEndpoint b{"10.0.0.1", 5001, ""};
    EXPECT_NE(a, b);
}

// ---------------------------------------------------------------------------
// JSON serialization
// ---------------------------------------------------------------------------

TEST(DiscoveryEndpoint, JsonRoundTripAllFields)
{
    DiscoveryEndpoint ep{"192.168.0.5", 9876, "server"};
    nlohmann::json    j  = ep;
    DiscoveryEndpoint ep2 = j.get<DiscoveryEndpoint>();

    EXPECT_EQ(ep2.IPAddress, "192.168.0.5");
    EXPECT_EQ(ep2.port, 9876);
    EXPECT_EQ(ep2.displayName, "server");
}

TEST(DiscoveryEndpoint, JsonRoundTripEmptyDisplayName)
{
    DiscoveryEndpoint ep{"1.2.3.4", 100, ""};
    nlohmann::json    j  = ep;
    DiscoveryEndpoint ep2 = j.get<DiscoveryEndpoint>();

    EXPECT_EQ(ep2.IPAddress, "1.2.3.4");
    EXPECT_EQ(ep2.displayName, "");
}

TEST(DiscoveryEndpoint, JsonKeys)
{
    DiscoveryEndpoint ep{"10.0.0.1", 5555, "pc"};
    nlohmann::json    j = ep;

    EXPECT_TRUE(j.contains("ip"));
    EXPECT_TRUE(j.contains("port"));
    EXPECT_TRUE(j.contains("name"));
}

TEST(DiscoveryEndpoint, FromJsonMissingNameField)
{
    // The "name" field is optional in from_json — must not throw
    nlohmann::json    j = {{"ip", "1.2.3.4"}, {"port", 42}};
    DiscoveryEndpoint ep;
    EXPECT_NO_THROW(ep = j.get<DiscoveryEndpoint>());
    EXPECT_EQ(ep.IPAddress, "1.2.3.4");
    EXPECT_EQ(ep.port, 42);
    EXPECT_EQ(ep.displayName, "");
}
