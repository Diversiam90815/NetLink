/*
  ==============================================================================
	Module:         NetworkInformation
	Description:    Platform-independent bookkeeping shared by all
					NetworkInformation<Platform> backends.
  ==============================================================================
*/

#include "NetworkInformation.h"

#include <algorithm>
#include <ranges>


bool netlink::NetworkInformation::setCurrentNetworkAdapter(const int adapterID)
{
	const auto it = std::ranges::find_if(mNetworkAdapters, [adapterID](const NetworkAdapterInternal &a) { return a.ID == adapterID; });

	if (it == mNetworkAdapters.end())
	{
		NETLINK_LOG_WARNING("No adapter found with ID {}", adapterID);
		return false;
	}

	return setCurrentNetworkAdapter(*it);
}


bool netlink::NetworkInformation::setCurrentNetworkAdapter(const NetworkAdapterInternal &adapter)
{
	if (mCurrentNetworkAdapter == adapter)
		return false;

	mCurrentNetworkAdapter = adapter;

	NETLINK_LOG_INFO("Set user defined adapter to :");
	NETLINK_LOG_INFO("\t Adapter:\t {}", adapter.AdapterName);
	NETLINK_LOG_INFO("\t IPv4: \t\t\t{}", adapter.IPv4);
	NETLINK_LOG_INFO("\t Subnet: \t\t{}", adapter.Subnet);
	NETLINK_LOG_INFO("\t ID: \t\t\t{}", adapter.ID);

	if (mOnAdapterChanged)
		mOnAdapterChanged(adapter.IPv4);

	return true;
}


const netlink::NetworkAdapterInternal &netlink::NetworkInformation::getCurrentNetworkAdapter() const
{
	return mCurrentNetworkAdapter;
}


netlink::NetworkAdapterInternal netlink::NetworkInformation::isAdapterCurrentlyAvailable(const NetworkAdapterInternal &adapter)
{
	// If the adapter is available we return the adapter current version (with maybe a new ID set)
	for (auto &it : mNetworkAdapters)
	{
		if (it == adapter)
			return it;
	}

	return {};
}


const std::vector<netlink::NetworkAdapterInternal> &netlink::NetworkInformation::getAvailableNetworkAdapters() const
{
	return mNetworkAdapters;
}
