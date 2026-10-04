/*
  ==============================================================================
	Module:         NetworkInformation
	Description:    Information about the local Network setup
  ==============================================================================
*/

#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <functional>
#include <string>
#include <string_view>
#include <tuple>

#include "Socket/IPv4Address.h"


namespace netlink
{

using AdapterChangedCallback = std::function<void(const std::string &newIPv4)>;

enum class AdapterTypes
{
	Ethernet = 1,
	WiFi	 = 2,
	Loopback = 3,
	Virtual	 = 4,
	Other	 = 5
};


enum class AdapterPriorityInternal
{
	Suppressed = 1,
	Available  = 2,
	Preferred  = 3
};


struct NetworkAdapterInternal
{
	NetworkAdapterInternal() = default;

	NetworkAdapterInternal(const std::string &adapterName, const std::string &networkName, const std::string &ipv4, const std::string &subnet, const uint64_t id,
						   bool isDefaultRoute, AdapterTypes type, AdapterPriorityInternal priority)
		: AdapterName(adapterName), NetworkName(networkName), IPv4(ipv4), Subnet(subnet), ID(id), IsDefaultRoute(isDefaultRoute), Type(type), Priority(priority)
	{
		Eligible = filterSubnetMask();
	}


	bool operator==(const NetworkAdapterInternal &other) const { return std::tie(AdapterName, Subnet) == std::tie(other.AdapterName, other.Subnet); }
	bool operator!=(const NetworkAdapterInternal &other) const { return !(*this == other); }

	bool isValid() const { return !AdapterName.empty() && !IPv4.empty() && ID != 0; }

	bool filterSubnetMask() const
	{
		const auto mask = netlink::net::IPv4Address::parse(Subnet);
		return mask.has_value() && mask->isNetmask();
	}

	std::string				AdapterName{};
	std::string				NetworkName{};
	std::string				IPv4{};
	std::string				Subnet{};
	uint64_t				ID{0}; // the same for as long as the interface keeps this address
	bool					IsDefaultRoute{false};
	bool					Eligible{false};
	AdapterTypes			Type{AdapterTypes::Other};
	AdapterPriorityInternal Priority{};
};



// One ID per interface and address: FNV-1a over both, never 0. interfaceKey is what the operating system
// identifies the interface by (LUID, interface index).
constexpr uint64_t makeAdapterId(const uint64_t interfaceKey, const std::string_view ipv4)
{
	uint64_t   hash = 0xCBF29CE484222325ull;
	const auto mix	= [&hash](const uint8_t byte)
	{
		hash ^= byte;
		hash *= 0x100000001B3ull;
	};

	for (int shift = 0; shift < 64; shift += 8)
		mix(static_cast<uint8_t>(interfaceKey >> shift));

	for (const char c : ipv4)
		mix(static_cast<uint8_t>(c));

	return hash != 0 ? hash : 1;
}


// What an adapter is shown as. Nothing is looked up for it: neither the name of the wireless network nor that of the gateway.
inline std::string networkNameOf(const AdapterTypes type, const std::string &ipv4)
{
	switch (type)
	{
	case AdapterTypes::WiFi: return "WiFi (" + ipv4 + ")";
	case AdapterTypes::Ethernet: return "Ethernet (" + ipv4 + ")";
	default: return {};
	}
}


class NetworkInformation
{
public:
	NetworkInformation();
	~NetworkInformation();

	bool									   init() const;

	void									   deinit();

	void									   processAdapter();

	bool									   setCurrentNetworkAdapter(const uint64_t adapterID);
	bool									   setCurrentNetworkAdapter(const NetworkAdapterInternal &adapter);
	const NetworkAdapterInternal			  &getCurrentNetworkAdapter() const;

	NetworkAdapterInternal					   isAdapterCurrentlyAvailable(const NetworkAdapterInternal &adapter);

	const std::vector<NetworkAdapterInternal> &getAvailableNetworkAdapters() const;

	void									   setOnAdapterChanged(AdapterChangedCallback cb) { mOnAdapterChanged = std::move(cb); }

private:
	// Platform-specific implementation, defined in NetworkInformation<Platform>.cpp/.mm
	struct Impl;
	std::unique_ptr<Impl>				mImpl;

	std::vector<NetworkAdapterInternal> mNetworkAdapters{};
	NetworkAdapterInternal				mCurrentNetworkAdapter{};

	AdapterChangedCallback				mOnAdapterChanged;
};


} // namespace netlink
