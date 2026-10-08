/*
  ==============================================================================
	Module:         DiscoveryLogic
	Description:    Which peers announced themselves on the network, and when
					this engine announces itself. Without I/O and without a
					clock: the engine feeds it beacons and the time.
  ==============================================================================
*/

#include "DiscoveryLogic.h"

#include <ranges>


namespace netlink::discovery
{

void DiscoveryLogic::setInterface(const net::IPv4Address &ip, const net::IPv4Address &mask)
{
	mLocalIp = ip;
	mMask	 = mask;
}


std::vector<PeerId> DiscoveryLogic::clear()
{
	std::vector<PeerId> known;
	known.reserve(mPeers.size());

	for (const auto &id : mPeers | std::views::keys)
		known.push_back(id);

	mPeers.clear();
	return known;
}


bool DiscoveryLogic::isInSubnet(const net::IPv4Address &address) const
{
	return !mMask.isNetmask() || net::sameSubnet(mLocalIp, address, mMask);
}


net::IPv4Address DiscoveryLogic::broadcastAddress() const
{
	return mMask.isNetmask() ? net::subnetBroadcast(mLocalIp, mMask) : net::IPv4Address::broadcast();
}


std::vector<uint8_t> DiscoveryLogic::beacon(const bool reply) const
{
	return encodeBeacon({.instanceId = mIdentity.instanceId, .appIdHash = mIdentity.appIdHash, .version = mIdentity.version, .reply = reply, .name = mIdentity.name});
}


DiscoveryLogic::Seen DiscoveryLogic::onBeacon(const net::SocketAddress &from, const std::span<const uint8_t> datagram, const TimePoint now)
{
	const auto beacon = decodeBeacon(datagram);

	// Everything is checked before any state is created for the sender
	if (!beacon || beacon->instanceId == mIdentity.instanceId || beacon->appIdHash != mIdentity.appIdHash || beacon->version != mIdentity.version || !isInSubnet(from.ip) ||
		beacon->name.size() > MaxDisplayName)
		return {};

	const PeerId id{beacon->instanceId};
	auto		 it		 = mPeers.find(id);
	const bool	 isNew	 = it == mPeers.end();
	const bool	 changed = !isNew && (it->second.peer.endpoint != from || it->second.peer.info.displayName != beacon->name);

	if (isNew)
	{
		if (mPeers.size() >= internal::MaxDiscoveredPeers)
			return {};

		it = mPeers.try_emplace(id).first;
	}

	Entry &entry   = it->second;
	entry.lastSeen = now;

	if (isNew || changed)
	{
		entry.peer.endpoint = from;
		entry.peer.info		= {.id = id, .displayName = beacon->name, .address = from.ip.toString(), .port = from.port, .appVersion = beacon->version.toString()};
	}

	return {.sighting = isNew || changed ? Sighting::New : Sighting::Known, .peer = &entry.peer, .reply = isNew && mAnnouncing && !beacon->reply};
}


std::vector<PeerId> DiscoveryLogic::expire(const TimePoint now, const std::function<bool(PeerId)> &hasSession)
{
	std::vector<PeerId> lost;

	for (auto it = mPeers.begin(); it != mPeers.end();)
	{
		if (now - it->second.lastSeen < internal::PeerExpiry || hasSession(it->first))
		{
			++it;
			continue;
		}

		lost.push_back(it->first);
		it = mPeers.erase(it);
	}

	return lost;
}


const KnownPeer *DiscoveryLogic::find(const PeerId id) const
{
	const auto it = mPeers.find(id);
	return it != mPeers.end() ? &it->second.peer : nullptr;
}


std::vector<PeerInfo> DiscoveryLogic::peers() const
{
	std::vector<PeerInfo> result;
	result.reserve(mPeers.size());

	for (const auto &entry : mPeers | std::views::values)
		result.push_back(entry.peer.info);

	return result;
}

} // namespace netlink::discovery
