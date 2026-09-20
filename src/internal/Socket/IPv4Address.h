/*
==============================================================================
	Module:         IPv4Address
	Description:    Validated IPv4 address. An instance can only be created
					through parse() or fromHostOrder(), so an object of this
					type always holds a well-formed address.
  ==============================================================================
*/

#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>


namespace netlink::net
{

class IPv4Address
{
public:
	// Default is the unspecified address 0.0.0.0
	constexpr IPv4Address() = default;

	// Strict dotted-quad parse: exactly four octets, decimal digits only, each 0..255,
	// no surrounding whitespace and no leading zeros (which would be ambiguously octal).
	static constexpr std::optional<IPv4Address> parse(std::string_view text)
	{
		uint32_t	 value	= 0;
		int			 octets = 0;
		size_t		 index	= 0;
		const size_t size	= text.size();

		while (octets < 4)
		{
			if (index >= size || text[index] < '0' || text[index] > '9')
				return std::nullopt;

			const bool leadingZero = text[index] == '0';

			uint32_t   octet	   = 0;
			size_t	   digits	   = 0;

			while (index < size && text[index] >= '0' && text[index] <= '9')
			{
				octet = octet * 10 + static_cast<uint32_t>(text[index] - '0');
				++index;
				++digits;

				if (octet > 255 || digits > 3)
					return std::nullopt;
			}

			// "0" is fine, "01" is not
			if (leadingZero && digits > 1)
				return std::nullopt;

			value = (value << 8) | octet;
			++octets;

			if (octets < 4)
			{
				if (index >= size || text[index] != '.')
					return std::nullopt;
				++index; // consume the separator
			}
		}

		// Trailing characters make the whole input invalid
		if (index != size)
			return std::nullopt;

		return IPv4Address(value);
	}

	static constexpr IPv4Address fromHostOrder(uint32_t value) { return IPv4Address(value); }

	constexpr uint32_t			 toHostOrder() const { return mValue; }

	std::string					 toString() const
	{
		return std::to_string((mValue >> 24) & 0xFF) + '.' + std::to_string((mValue >> 16) & 0xFF) + '.' + std::to_string((mValue >> 8) & 0xFF) + '.' +
			   std::to_string(mValue & 0xFF);
	}

	constexpr bool				 isUnspecified() const { return mValue == 0; }		   // 0.0.0.0
	constexpr bool				 isLoopback() const { return (mValue >> 24) == 127; }  // 127.0.0.0/8
	constexpr bool				 isBroadcast() const { return mValue == 0xFFFFFFFFu; } // 255.255.255.255

	constexpr auto				 operator<=>(const IPv4Address &other) const = default;
	constexpr bool				 operator==(const IPv4Address &other) const	 = default;

	static constexpr IPv4Address unspecified() { return IPv4Address(0); }
	static constexpr IPv4Address broadcast() { return IPv4Address(0xFFFFFFFFu); }

private:
	explicit constexpr IPv4Address(uint32_t value) : mValue(value) {}

	uint32_t mValue{0};
};


// Directed broadcast address of the subnet the address belongs to (e.g. 192.168.1.7/24 -> 192.168.1.255).
constexpr IPv4Address subnetBroadcast(IPv4Address address, IPv4Address mask)
{
	return IPv4Address::fromHostOrder(address.toHostOrder() | ~mask.toHostOrder());
}


// True when both addresses sit on the same subnet under the given mask.
constexpr bool sameSubnet(IPv4Address first, IPv4Address second, IPv4Address mask)
{
	return (first.toHostOrder() & mask.toHostOrder()) == (second.toHostOrder() & mask.toHostOrder());
}

} // namespace netlink::net
