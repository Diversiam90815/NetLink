#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Discovery/Beacon.h"

using namespace netlink;
using namespace netlink::discovery;


namespace DiscoveryTests
{

static Beacon sample()
{
	return {.instanceId = 0x0102030405060708ull, .appIdHash = hashAppId("my-app"), .version = {3, 14}, .reply = false, .name = "Living room"};
}


TEST(BeaconTest, AppIdHash_IsFnv1a64)
{
	// Published test vectors of FNV-1a, 64 bit
	EXPECT_EQ(hashAppId(""), 0xCBF29CE484222325ull);
	EXPECT_EQ(hashAppId("a"), 0xAF63DC4C8601EC8Cull);
	EXPECT_EQ(hashAppId("foobar"), 0x85944171F73967E8ull);

	EXPECT_NE(hashAppId("my-app"), hashAppId("my-app/group-2")) << "Groups are part of the appId";
}


TEST(BeaconTest, AppVersion_PatchIgnored)
{
	EXPECT_EQ(parseAppVersion("1.4.0.100"), parseAppVersion("1.4.9.2000"));
	EXPECT_EQ(parseAppVersion("1.4"), (AppVersion{1, 4}));
	EXPECT_NE(parseAppVersion("1.4.0"), parseAppVersion("1.5.0"));
	EXPECT_NE(parseAppVersion("1.4.0"), parseAppVersion("2.4.0"));

	EXPECT_EQ(parseAppVersion("7"), (AppVersion{7, 0}));
	EXPECT_EQ(parseAppVersion(""), (AppVersion{0, 0}));
	EXPECT_EQ(parseAppVersion("abc.def"), (AppVersion{0, 0})) << "What cannot be read counts as 0";
	EXPECT_EQ(parseAppVersion("2.x"), (AppVersion{2, 0}));
	EXPECT_EQ(parseAppVersion("1.4").toString(), "1.4");
}


TEST(BeaconTest, RoundTrip)
{
	Beacon beacon	   = sample();
	beacon.reply	   = true;

	const auto bytes   = encodeBeacon(beacon);
	const auto decoded = decodeBeacon(bytes);

	ASSERT_TRUE(decoded.has_value());
	EXPECT_EQ(decoded->instanceId, beacon.instanceId);
	EXPECT_EQ(decoded->appIdHash, beacon.appIdHash);
	EXPECT_EQ(decoded->version, beacon.version);
	EXPECT_TRUE(decoded->reply);
	EXPECT_EQ(decoded->name, "Living room");
	EXPECT_TRUE(isBeacon(bytes));
}


TEST(BeaconTest, Layout_IsFixed)
{
	const auto bytes = encodeBeacon(sample());

	ASSERT_EQ(bytes.size(), 4u + 8 + 8 + 2 + 2 + 1 + 1 + 11 + 2);
	EXPECT_EQ(bytes[0], 0x4E);
	EXPECT_EQ(bytes[1], 0x4C);
	EXPECT_EQ(bytes[2], 3) << "protocol version";
	EXPECT_EQ(bytes[3], 3) << "kind Beacon, nothing else";
	EXPECT_EQ(bytes[4], 0x01) << "big endian";
	EXPECT_EQ(bytes[11], 0x08);
	EXPECT_EQ(bytes[21], 3);
	EXPECT_EQ(bytes[23], 14);
	EXPECT_EQ(bytes[24], 0) << "not a reply";
	EXPECT_EQ(bytes[25], 11) << "length of the name";
}


TEST(BeaconTest, UnknownExtTlv_IsSkipped)
{
	auto					   bytes = encodeBeacon(sample());

	// Two fields of types this version does not know, 3 and 0 bytes long
	const std::vector<uint8_t> ext{0x70, 3, 1, 2, 3, 0x71, 0};
	bytes[bytes.size() - 1] = static_cast<uint8_t>(ext.size());
	bytes.insert(bytes.end(), ext.begin(), ext.end());

	const auto decoded = decodeBeacon(bytes);
	ASSERT_TRUE(decoded.has_value()) << "A later version may say more: this one still reads what it knows";
	EXPECT_EQ(decoded->name, "Living room");
}


TEST(BeaconTest, MalformedBeacons_AreRejected)
{
	const auto good = encodeBeacon(sample());
	ASSERT_TRUE(decodeBeacon(good).has_value());

	for (size_t length = 0; length < good.size(); ++length)
		EXPECT_FALSE(decodeBeacon(std::span(good).first(length)).has_value()) << "cut off after " << length << " bytes";

	auto trailing = good;
	trailing.push_back(0);
	EXPECT_FALSE(decodeBeacon(trailing).has_value()) << "No bytes without a meaning";

	auto wrongVersion = good;
	wrongVersion[2]	  = 2;
	EXPECT_FALSE(decodeBeacon(wrongVersion).has_value());
	EXPECT_FALSE(isBeacon(wrongVersion));

	auto wrongKind = good;
	wrongKind[3]   = 0;
	EXPECT_FALSE(decodeBeacon(wrongKind).has_value());

	auto unknownFlag = good;
	unknownFlag[24]	 = 0x02;
	EXPECT_FALSE(decodeBeacon(unknownFlag).has_value());

	auto brokenExt					= good;
	brokenExt[brokenExt.size() - 1] = 2;
	brokenExt.push_back(0x70);
	brokenExt.push_back(5); // a field that claims more bytes than there are
	EXPECT_FALSE(decodeBeacon(brokenExt).has_value());

	Beacon anonymous	 = sample();
	anonymous.instanceId = 0;
	EXPECT_FALSE(decodeBeacon(encodeBeacon(anonymous)).has_value()) << "Every engine has an ID";
}

} // namespace DiscoveryTests
