/*
  ==============================================================================
	Module:         NetworkManager
	Description:    Managing the network part of this app
  ==============================================================================
*/

#include "NetworkManager.h"


void NetworkManager::init()
{
	mNetworkInfo.init();

	mNetworkInfo.processAdapter();

	setInitialized(true);
}


const std::vector<NetworkAdapter> &NetworkManager::getAvailableNetworkAdapters() const
{
	return mNetworkInfo.getAvailableNetworkAdapters();
}


void NetworkManager::networkAdapterChanged(const NetworkAdapter &adapter)
{
	auto &currentAdapter = mNetworkInfo.getCurrentNetworkAdapter();

	if (currentAdapter != adapter)
	{
		LOG_INFO("Network Adapter has been changed to {} with IP", adapter.AdapterName.c_str(), adapter.IPv4.c_str());

		mNetworkInfo.setCurrentNetworkAdapter(adapter);

		for (auto &observer : mObservers)
		{
			if (auto obs = observer.lock())
				obs->onNetworkAdapterChanged(adapter);
		}
	}
}


int NetworkManager::getCurrentNetworkAdapterID()
{
	return mNetworkInfo.getCurrentNetworkAdapter().ID;
}


const std::string &NetworkManager::getCurrentIPv4()
{
	return mNetworkInfo.getCurrentNetworkAdapter().IPv4;
}


bool NetworkManager::changeCurrentNetworkAdapter(const int ID)
{
	auto &adapters = getAvailableNetworkAdapters();

	for (auto &adapter : adapters)
	{
		if (adapter.ID != ID)
			continue;

		networkAdapterChanged(adapter);
		return true;
	}
	return false;
}
