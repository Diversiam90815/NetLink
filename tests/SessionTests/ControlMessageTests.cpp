#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Session/ControlMessage.h"

using namespace netlink::session;


namespace SessionTests
{

TEST(ControlMessageTest, Hello_RoundTrip)
{
	const Hello hello{.fromInstance = 0x1111222233334444ull, .toInstance = 0x5555666677778888ull, .appIdHash = 42, .verMajor = 1, .verMinor = 7, .name = "Studio"};

	const auto	bytes	= encode(hello);
	const auto	decoded = decodeHello(bytes);

	ASSERT_TRUE(decoded.has_value());
	EXPECT_EQ(decoded->fromInstance, hello.fromInstance);
	EXPECT_EQ(decoded->toInstance, hello.toInstance);
	EXPECT_EQ(decoded->appIdHash, 42u);
	EXPECT_EQ(decoded->verMajor, 1);
	EXPECT_EQ(decoded->verMinor, 7);
	EXPECT_EQ(decoded->name, "Studio");

	EXPECT_EQ(bytes.size(), 8u + 8 + 8 + 2 + 2 + 1 + 6 + 2);
	EXPECT_EQ(bytes[0], 0x11) << "big endian";
}


TEST(ControlMessageTest, Hello_Malformed_IsRejected)
{
	const auto good = encode(Hello{.fromInstance = 1, .toInstance = 2, .appIdHash = 3, .verMajor = 1, .verMinor = 0, .name = "x"});

	for (size_t length = 0; length < good.size(); ++length)
		EXPECT_FALSE(decodeHello(std::span(good).first(length)).has_value()) << "cut off after " << length << " bytes";

	auto trailing = good;
	trailing.push_back(0);
	EXPECT_FALSE(decodeHello(trailing).has_value());

	EXPECT_FALSE(decodeHello(encode(Hello{.fromInstance = 0, .toInstance = 2, .appIdHash = 3, .verMajor = 1, .verMinor = 0, .name = "x"})).has_value()) << "Whoever asks has an ID";
}


TEST(ControlMessageTest, Hello_UnknownExtFields_AreSkipped)
{
	auto				 bytes = encode(Hello{.fromInstance = 1, .toInstance = 2, .appIdHash = 3, .verMajor = 1, .verMinor = 0, .name = "x"});

	// What a build with encryption will add: a 32 byte key and a cipher suite
	std::vector<uint8_t> ext{1, 32};
	ext.resize(34, 0xAB);
	ext.push_back(2);
	ext.push_back(1);
	ext.push_back(7);

	bytes[bytes.size() - 1] = static_cast<uint8_t>(ext.size());
	bytes.insert(bytes.end(), ext.begin(), ext.end());

	const auto decoded = decodeHello(bytes);
	ASSERT_TRUE(decoded.has_value());
	EXPECT_EQ(decoded->name, "x");
}


TEST(ControlMessageTest, Accept_RoundTrip)
{
	const auto decoded = decodeAccept(encode(Accept{.name = "Studio"}));
	ASSERT_TRUE(decoded.has_value());
	EXPECT_EQ(decoded->name, "Studio");

	EXPECT_FALSE(decodeAccept(std::vector<uint8_t>{}).has_value());
	EXPECT_FALSE(decodeAccept(std::vector<uint8_t>{3, 'a'}).has_value());
}


TEST(ControlMessageTest, Decline_RoundTrip)
{
	const auto decoded = decodeDecline(encode(Decline{.reason = DeclineReason::Incompatible, .text = "needs encryption"}));
	ASSERT_TRUE(decoded.has_value());
	EXPECT_EQ(decoded->reason, DeclineReason::Incompatible);
	EXPECT_EQ(decoded->text, "needs encryption");

	EXPECT_EQ(decodeDecline(std::vector<uint8_t>{200, 0})->reason, DeclineReason::Declined) << "A reason of a later version is still a decline";
	EXPECT_FALSE(decodeDecline(std::vector<uint8_t>{}).has_value());
}


TEST(ControlMessageTest, Text_IsCutOffAtItsLengthByte)
{
	const auto decoded = decodeAccept(encode(Accept{.name = std::string(300, 'n')}));
	ASSERT_TRUE(decoded.has_value());
	EXPECT_EQ(decoded->name.size(), 255u);
}

} // namespace SessionTests
