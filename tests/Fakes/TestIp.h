/*
  ==============================================================================
	Module:         TestIp
	Description:    Shorthand for building a validated IPv4Address from a
					literal in tests. Throws on a malformed literal, which is a
					bug in the test itself.
  ==============================================================================
*/

#pragma once

#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "Socket/IPv4Address.h"


inline netlink::net::IPv4Address ipv4(std::string_view text)
{
	auto parsed = netlink::net::IPv4Address::parse(text);

	if (!parsed.has_value())
		throw std::invalid_argument("test used a malformed IPv4 literal: " + std::string(text));

	return *parsed;
}


// Makes gtest print a readable address instead of a byte dump on failure
namespace netlink::net
{
inline void PrintTo(const IPv4Address &address, std::ostream *out)
{
	*out << address.toString();
}
} // namespace netlink::net
