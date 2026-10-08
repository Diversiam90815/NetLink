/*
==============================================================================
	Module:         NetLink
	Description:    API for NetLink library
  ==============================================================================
*/

#include "NetLink/NetLink.h"

#include "Core/NetLinkCore.h"


struct netlink::NetLink::Impl
{
	NetLinkCore core;
};


netlink::NetLink::NetLink() : pImpl(std::make_unique<Impl>()) {}


netlink::NetLink::~NetLink() = default;


bool netlink::NetLink::start(const NetLinkConfig &config, const NetLinkCallbacks &callbacks)
{
	return pImpl->core.start(config, callbacks);
}


void netlink::NetLink::stop()
{
	pImpl->core.stop();
}


// ---------------------------------------------------------------------------
// Discovery & sessions
// ---------------------------------------------------------------------------

bool netlink::NetLink::startDiscovery()
{
	return pImpl->core.startDiscovery();
}


void netlink::NetLink::stopDiscovery()
{
	pImpl->core.stopDiscovery();
}


std::vector<netlink::PeerInfo> netlink::NetLink::peers() const
{
	return pImpl->core.peers();
}


bool netlink::NetLink::connect(const PeerId peer)
{
	return pImpl->core.connect(peer);
}


void netlink::NetLink::accept(const PeerId peer)
{
	pImpl->core.accept(peer);
}


void netlink::NetLink::decline(const PeerId peer)
{
	pImpl->core.decline(peer);
}


void netlink::NetLink::disconnect(const PeerId peer)
{
	pImpl->core.disconnect(peer);
}


std::vector<netlink::PeerId> netlink::NetLink::connectedPeers() const
{
	return pImpl->core.connectedPeers();
}


// ---------------------------------------------------------------------------
// Messaging
// ---------------------------------------------------------------------------

netlink::SendResult netlink::NetLink::send(const PeerId peer, const uint32_t type, const std::span<const uint8_t> data, const Lane lane, const std::chrono::milliseconds timeout)
{
	return pImpl->core.send(peer, type, std::vector<uint8_t>(data.begin(), data.end()), lane, timeout);
}


netlink::SendResult netlink::NetLink::send(const PeerId peer, const uint32_t type, std::vector<uint8_t> &&data, const Lane lane, const std::chrono::milliseconds timeout)
{
	return pImpl->core.send(peer, type, std::move(data), lane, timeout);
}


size_t netlink::NetLink::broadcast(const uint32_t type, const std::span<const uint8_t> data, const Lane lane)
{
	return pImpl->core.broadcast(type, data, lane);
}


std::optional<netlink::PeerStats> netlink::NetLink::stats(const PeerId peer) const
{
	return pImpl->core.stats(peer);
}


// ---------------------------------------------------------------------------
// Network adapters
// ---------------------------------------------------------------------------

std::vector<netlink::NetworkAdapter> netlink::NetLink::getAvailableAdapters() const
{
	return pImpl->core.getAvailableAdapters();
}


bool netlink::NetLink::setActiveAdapter(const uint64_t adapterID)
{
	return pImpl->core.setActiveAdapter(adapterID);
}


uint64_t netlink::NetLink::getActiveAdapterID() const
{
	return pImpl->core.getActiveAdapterID();
}
