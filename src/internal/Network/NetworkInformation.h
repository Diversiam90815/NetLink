/*
  ==============================================================================
	Module:         NetworkInformation
	Description:    Information about the local Network setup
  ==============================================================================
*/

#pragma once

#include <vector>
#include <memory>
#include <functional>
#include <string>
#include <tuple>

#include "NetLinkLog.h"


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

	NetworkAdapterInternal(const std::string	  &adapterName,
						   const std::string	  &networkName,
						   const std::string	  &ipv4,
						   const std::string	  &subnet,
						   const int			   id,
						   bool					   isDefaultRoute,
						   AdapterTypes			   type,
						   AdapterPriorityInternal priority)
		: AdapterName(adapterName), NetworkName(networkName), IPv4(ipv4), Subnet(subnet), ID(id), IsDefaultRoute(isDefaultRoute), Type(type), Priority(priority)
	{
		Eligible = filterSubnetMask();
	}


	bool					operator==(const NetworkAdapterInternal &other) const { return std::tie(AdapterName, Subnet) == std::tie(other.AdapterName, other.Subnet); }
	bool					operator!=(const NetworkAdapterInternal &other) const { return !(*this == other); }

	bool					isValid() const { return !AdapterName.empty() && !IPv4.empty() && ID != 0; }

	bool					filterSubnetMask() const { return Subnet == "255.255.255.0"; }

	std::string				AdapterName{};
	std::string				NetworkName{};
	std::string				IPv4{};
	std::string				Subnet{};
	int						ID{0};
	bool					IsDefaultRoute{false};
	bool					Eligible{false};
	AdapterTypes			Type{AdapterTypes::Other};
	AdapterPriorityInternal Priority{};
};



class NetworkInformation
{
public:
	NetworkInformation();
	~NetworkInformation();

	bool									   init();

	void									   deinit();

	void									   processAdapter();

	bool									   setCurrentNetworkAdapter(const int adapterID);
	bool									   setCurrentNetworkAdapter(const NetworkAdapterInternal &adapter);
	const NetworkAdapterInternal			  &getCurrentNetworkAdapter() const;

	NetworkAdapterInternal					   isAdapterCurrentlyAvailable(const NetworkAdapterInternal &adapter);

	const std::vector<NetworkAdapterInternal> &getAvailableNetworkAdapters() const;

	void									   setOnAdapterChanged(AdapterChangedCallback cb) { mOnAdapterChanged = std::move(cb); }

private:
	// Platform-specific implementation, defined in NetworkInformation<Platform>.cpp/.mm
	struct Impl;
	std::unique_ptr<Impl>				 mImpl;

	std::vector<NetworkAdapterInternal> mNetworkAdapters{};
	NetworkAdapterInternal				 mCurrentNetworkAdapter{};

	AdapterChangedCallback				 mOnAdapterChanged;
};


} // namespace netlink
