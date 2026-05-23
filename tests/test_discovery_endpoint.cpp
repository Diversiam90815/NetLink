#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "DiscoveryEndpoint.h"

// ---------------------------------------------------------------------------
// isValid
// ---------------------------------------------------------------------------

TEST(DiscoveryEndpoint, IsValidWithIPAndPort)
{
    DiscoveryEndpoint ep{"192.168.1.1", 5000, "mypc"};
    EXPECT_TRUE(ep.isValid())
        << "An endpoint with a non-empty IP and non-zero port must be valid";
}

TEST(DiscoveryEndpoint, IsValidRequiresNonEmptyIP)
{
    DiscoveryEndpoint ep{"", 5000, "mypc"};
    EXPECT_FALSE(ep.isValid())
        << "An endpoint with an empty IP address must not be considered valid";
}

TEST(DiscoveryEndpoint, IsValidRequiresNonZeroPort)
{
    DiscoveryEndpoint ep{"192.168.1.1", 0, "mypc"};
    EXPECT_FALSE(ep.isValid())
        << "An endpoint with port 0 must not be considered valid";
}

// ---------------------------------------------------------------------------
// isEmpty
// ---------------------------------------------------------------------------

TEST(DiscoveryEndpoint, IsEmptyWhenDefaultConstructed)
{
    DiscoveryEndpoint ep{};
    EXPECT_TRUE(ep.isEmpty())
        << "A default-constructed endpoint must be empty (both IP and port at their zero values)";
}

TEST(DiscoveryEndpoint, IsNotEmptyWithIPOnly)
{
    DiscoveryEndpoint ep{"10.0.0.1", 0, ""};
    EXPECT_FALSE(ep.isEmpty())
        << "An endpoint with a non-empty IP must not be empty even when port is 0";
}

TEST(DiscoveryEndpoint, IsNotEmptyWithPortOnly)
{
    DiscoveryEndpoint ep{"", 1234, ""};
    EXPECT_FALSE(ep.isEmpty())
        << "An endpoint with a non-zero port must not be empty even when the IP is empty";
}

// ---------------------------------------------------------------------------
// operator==
// ---------------------------------------------------------------------------

TEST(DiscoveryEndpoint, EqualityMatchesIPAndPort)
{
    DiscoveryEndpoint a{"10.0.0.1", 5000, "alpha"};
    DiscoveryEndpoint b{"10.0.0.1", 5000, "beta"}; // different name — still equal
    EXPECT_EQ(a, b)
        << "Two endpoints with the same IP and port must be equal regardless of displayName";
}

TEST(DiscoveryEndpoint, InequalityDifferentIP)
{
    DiscoveryEndpoint a{"10.0.0.1", 5000, ""};
    DiscoveryEndpoint b{"10.0.0.2", 5000, ""};
    EXPECT_NE(a, b)
        << "Endpoints with different IP addresses must not be equal";
}

TEST(DiscoveryEndpoint, InequalityDifferentPort)
{
    DiscoveryEndpoint a{"10.0.0.1", 5000, ""};
    DiscoveryEndpoint b{"10.0.0.1", 5001, ""};
    EXPECT_NE(a, b)
        << "Endpoints with different ports must not be equal";
}

// ---------------------------------------------------------------------------
// JSON serialization
// ---------------------------------------------------------------------------

TEST(DiscoveryEndpoint, JsonRoundTripAllFields)
{
    DiscoveryEndpoint ep{"192.168.0.5", 9876, "server"};
    nlohmann::json    j   = ep;
    DiscoveryEndpoint ep2 = j.get<DiscoveryEndpoint>();

    EXPECT_EQ(ep2.IPAddress, "192.168.0.5")
        << "IPAddress must survive the JSON round-trip unchanged";
    EXPECT_EQ(ep2.port, 9876)
        << "port must survive the JSON round-trip unchanged";
    EXPECT_EQ(ep2.displayName, "server")
        << "displayName must survive the JSON round-trip unchanged";
}

TEST(DiscoveryEndpoint, JsonRoundTripEmptyDisplayName)
{
    DiscoveryEndpoint ep{"1.2.3.4", 100, ""};
    nlohmann::json    j   = ep;
    DiscoveryEndpoint ep2 = j.get<DiscoveryEndpoint>();

    EXPECT_EQ(ep2.IPAddress, "1.2.3.4")
        << "IPAddress must survive the round-trip even when displayName is empty";
    EXPECT_EQ(ep2.displayName, "")
        << "An empty displayName must deserialize back as empty";
}

TEST(DiscoveryEndpoint, JsonKeys)
{
    DiscoveryEndpoint ep{"10.0.0.1", 5555, "pc"};
    nlohmann::json    j = ep;

    EXPECT_TRUE(j.contains("ip"))
        << "Serialized JSON must contain the 'ip' key";
    EXPECT_TRUE(j.contains("port"))
        << "Serialized JSON must contain the 'port' key";
    EXPECT_TRUE(j.contains("name"))
        << "Serialized JSON must contain the 'name' key";
}

TEST(DiscoveryEndpoint, FromJsonMissingNameField)
{
    // The "name" field is optional in from_json — must not throw
    nlohmann::json    j = {{"ip", "1.2.3.4"}, {"port", 42}};
    DiscoveryEndpoint ep;
    EXPECT_NO_THROW(ep = j.get<DiscoveryEndpoint>())
        << "Deserializing a JSON object without a 'name' field must not throw";
    EXPECT_EQ(ep.IPAddress, "1.2.3.4")
        << "IPAddress must be read correctly even when 'name' is absent";
    EXPECT_EQ(ep.port, 42)
        << "port must be read correctly even when 'name' is absent";
    EXPECT_EQ(ep.displayName, "")
        << "displayName must default to empty when the 'name' key is absent from JSON";
}
