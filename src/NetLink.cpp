/*
==============================================================================
	Module:         NetLink
	Description:    API for NetLink library
  ==============================================================================
*/

#include "NetLink/NetLink.h"

#include <algorithm>

#include "Core/NetLinkCore.h"
#include "Network/NetworkInformation.h"


// The facade adds network adapter handling on top of the core, which owns and wires all services
struct netlink::NetLink::Impl
{
	NetLinkCore		   core;
	NetworkInformation network; // destroyed before the core its callback refers to
};


// ---------------------------------------------------------------------------
// Helpers: map internal <-> public types
// ---------------------------------------------------------------------------

static netlink::AdapterPriority mapPriority(const netlink::AdapterPriorityInternal internal)
{
	switch (internal)
	{
	case netlink::AdapterPriorityInternal::Preferred: return netlink::AdapterPriority::Preferred;
	case netlink::AdapterPriorityInternal::Available: return netlink::AdapterPriority::Available;
	default: return netlink::AdapterPriority::Suppressed;
	}
}

static netlink::NetworkAdapter toPublicAdapter(const netlink::NetworkAdapterInternal &internal)
{
	netlink::NetworkAdapter pub;
	pub.adapterName = internal.AdapterName;
	pub.networkName = internal.NetworkName;
	pub.ipv4		= internal.IPv4;
	pub.id			= internal.ID;
	pub.priority	= mapPriority(internal.Priority);
	return pub;
}


// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

netlink::NetLink::NetLink() : pImpl(std::make_unique<Impl>()) {}


netlink::NetLink::~NetLink()
{
	shutdown();
}


void netlink::NetLink::configure(const NetLinkConfig &config, const NetLinkCallbacks &callbacks) const
{
	pImpl->core.configure(config, callbacks);
}


bool netlink::NetLink::init() const
{
	Impl *impl = pImpl.get();

	// Adapter selected (by the app or automatically): move all networking to its address and tell the app
	impl->network.setOnAdapterChanged(
		[impl](const std::string &newIPv4)
		{
			impl->core.setLocalAddress(newIPv4, impl->network.getCurrentNetworkAdapter().Subnet);

			impl->core.postEvent(
				[adapter = toPublicAdapter(impl->network.getCurrentNetworkAdapter())](const NetLinkCallbacks &callbacks)
				{
					if (callbacks.onNetworkAdapterChanged)
						callbacks.onNetworkAdapterChanged(adapter);
				});
		});

	if (!impl->network.init())
		return false;

	impl->network.processAdapter();

	if (!impl->core.init())
		return false;

	if (const auto &current = impl->network.getCurrentNetworkAdapter(); current.isValid())
	{
		impl->core.setLocalAddress(current.IPv4, current.Subnet);
		return true;
	}

	// No adapter chosen yet: default to the best candidate, the app can still switch via setActiveAdapter()
	const auto &adapters  = impl->network.getAvailableNetworkAdapters();
	auto		preferred = std::ranges::find_if(adapters, [](const auto &a) { return a.isValid() && a.Priority == AdapterPriorityInternal::Preferred; });

	if (preferred == adapters.end())
		preferred = std::ranges::find_if(adapters, [](const auto &a) { return a.isValid() && a.Priority == AdapterPriorityInternal::Available; });

	if (preferred != adapters.end())
		impl->network.setCurrentNetworkAdapter(*preferred);

	return true;
}


void netlink::NetLink::shutdown() const
{
	pImpl->core.shutdown();
}


// ---------------------------------------------------------------------------
// Discovery & connection
// ---------------------------------------------------------------------------

bool netlink::NetLink::startDiscovery() const
{
	return pImpl->core.startDiscovery();
}


void netlink::NetLink::stopDiscovery() const
{
	pImpl->core.stopDiscovery();
}


std::vector<netlink::Endpoint> netlink::NetLink::getPotentialEndpoints() const
{
	return pImpl->core.getPotentialEndpoints();
}


bool netlink::NetLink::connectTo(const Endpoint &remote) const
{
	return pImpl->core.connectTo(remote);
}


void netlink::NetLink::respondToConnection(const bool accepted) const
{
	pImpl->core.respondToConnection(accepted);
}


void netlink::NetLink::disconnect() const
{
	pImpl->core.disconnect();
}


netlink::ConnectionState netlink::NetLink::getConnectionState() const
{
	return pImpl->core.getConnectionState();
}


// ---------------------------------------------------------------------------
// Messaging
// ---------------------------------------------------------------------------

bool netlink::NetLink::send(const Message &message, const DeliveryMode mode) const
{
	return pImpl->core.send(message.type, message.data, mode);
}


bool netlink::NetLink::send(const uint32_t type, const std::vector<uint8_t> &payload, const DeliveryMode mode) const
{
	return pImpl->core.send(type, payload, mode);
}


// ---------------------------------------------------------------------------
// Network adapters
// ---------------------------------------------------------------------------

std::vector<netlink::NetworkAdapter> netlink::NetLink::getAvailableAdapters() const
{
	const auto				   &internal = pImpl->network.getAvailableNetworkAdapters();

	std::vector<NetworkAdapter> result;
	result.reserve(internal.size());

	for (const auto &adapter : internal)
		result.push_back(toPublicAdapter(adapter));

	return result;
}


bool netlink::NetLink::setActiveAdapter(const int &adapterID) const
{
	// Fires onAdapterChanged, which moves all networking to the new address
	return pImpl->network.setCurrentNetworkAdapter(adapterID);
}


int netlink::NetLink::getActiveAdapterID() const
{
	return pImpl->network.getCurrentNetworkAdapter().ID;
}
