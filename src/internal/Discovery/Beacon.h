/*
  ==============================================================================
	Module:         Beacon
	Description:    The datagram an engine announces itself with, and what both
					sides of it compare: the application and its version
  ==============================================================================
*/

#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Channel/Protocol/PacketHeader.h"
#include "Channel/Protocol/WireBuffer.h"
#include "Session/ControlMessage.h"


namespace netlink::discovery
{

// FNV-1a, 64 bit: the same on every platform and standard library, unlike std::hash
constexpr uint64_t hashAppId(const std::string_view appId)
{
	uint64_t hash = 0xCBF29CE484222325ull;

	for (const char c : appId)
	{
		hash ^= static_cast<uint8_t>(c);
		hash *= 0x100000001B3ull;
	}

	return hash;
}


// Two applications are compatible when major and minor match: patch and build number are ignored
struct AppVersion
{
	uint16_t	verMajor{0};
	uint16_t	verMinor{0};

	bool		operator==(const AppVersion &) const = default;

	std::string toString() const { return std::to_string(verMajor) + "." + std::to_string(verMinor); }
};

// major.minor of a dotted version string. Missing or unparsable components count as 0.
inline AppVersion parseAppVersion(const std::string_view version)
{
	const auto number = [](const std::string_view text)
	{
		uint16_t value		 = 0;
		const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
		return ec == std::errc{} ? value : uint16_t{0};
	};

	const auto	   firstDot = version.find('.');
	const uint16_t major	= number(version.substr(0, firstDot));

	if (firstDot == std::string_view::npos)
		return {.verMajor = major, .verMinor = 0};

	const auto rest = version.substr(firstDot + 1);
	return {.verMajor = major, .verMinor = number(rest.substr(0, rest.find('.')))};
}


/*
 Beacon encoding, after the prefix every datagram starts with (magic, version, flags with kind Beacon):

	4   u64   instanceId
	12  u64   appIdHash
	20  u16   verMajor
	22  u16   verMinor
	24  u8    flags             (bit 0: Reply, the answer to a beacon that was seen for the first time)
	25  u8    name length, name
		u16   ext length, ext   (see ControlMessage.h)

 Where the sender can be reached is not part of it: that is the address the datagram came from.
 */
struct Beacon
{
	uint64_t	instanceId{0};
	uint64_t	appIdHash{0};
	AppVersion	version;
	bool		reply{false};
	std::string name;
};

inline constexpr size_t	 BeaconPrefixSize = 4;
inline constexpr uint8_t BeaconReplyFlag  = 0x01;


// Whether the datagram claims to be a beacon of this protocol version
inline bool				 isBeacon(const std::span<const uint8_t> datagram)
{
	return datagram.size() >= BeaconPrefixSize && channel::readUint16(datagram.data()) == channel::ProtocolMagic && datagram[2] == channel::ProtocolVersion &&
		   datagram[3] == std::to_underlying(channel::PacketKind::Beacon);
}


inline std::vector<uint8_t> encodeBeacon(const Beacon &beacon)
{
	channel::WireWriter out;
	out.u16(channel::ProtocolMagic);
	out.u8(channel::ProtocolVersion);
	out.u8(std::to_underlying(channel::PacketKind::Beacon));
	out.u64(beacon.instanceId);
	out.u64(beacon.appIdHash);
	out.u16(beacon.version.verMajor);
	out.u16(beacon.version.verMinor);
	out.u8(beacon.reply ? BeaconReplyFlag : uint8_t{0});
	out.text(beacon.name);
	session::writeExt(out);
	return out.take();
}


inline std::optional<Beacon> decodeBeacon(const std::span<const uint8_t> datagram)
{
	if (!isBeacon(datagram) || datagram.size() > internal::MaxDatagramSize)
		return std::nullopt;

	channel::WireReader in(datagram.subspan(BeaconPrefixSize));
	Beacon				beacon;
	beacon.instanceId		= in.u64();
	beacon.appIdHash		= in.u64();
	beacon.version.verMajor = in.u16();
	beacon.version.verMinor = in.u16();

	const uint8_t flags		= in.u8();
	beacon.reply			= (flags & BeaconReplyFlag) != 0;
	beacon.name				= in.text();

	if ((flags & ~BeaconReplyFlag) != 0 || !session::skipExt(in) || !in.complete() || beacon.instanceId == 0)
		return std::nullopt;

	return beacon;
}

} // namespace netlink::discovery
