/*
  ==============================================================================
	Module:         NetworkInformation
	Description:    Platform-independent bookkeeping shared by all
					NetworkInformation<Platform> backends.
  ==============================================================================
*/

#include "NetworkInformation.h"


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
