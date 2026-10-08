/*
  ==============================================================================
	Module:         DiscoveryLogic
	Description:    Which peers announced themselves on the network, and when
					this engine announces itself. Without I/O and without a
					clock: the engine feeds it beacons and the time.
  ==============================================================================
*/

#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "NetLink/NetLink.h"

#include "Beacon.h"
#include "Socket/SocketTypes.h"
#include "TransportConstants.h"


namespace netlink::discovery
{

struct LocalIdentity
{
	uint64_t	instanceId{0};
	uint64_t	appIdHash{0};
	AppVersion	version;
	std::string name;
};


struct KnownPeer
{
	PeerInfo		   info;
	net::SocketAddress endpoint; // of its channel socket: where its beacon came from
};


class DiscoveryLogic
{
public:
	using Clock		= std::chrono::steady_clock;
	using TimePoint = Clock::time_point;

	enum class Sighting
	{
		Ignored, // not a beacon, an own one, or one of another application, version or subnet
		Known,
		New,	 // seen for the first time, or at a new address
	};

	struct Seen
	{
		Sighting		 sighting{Sighting::Ignored};
		const KnownPeer *peer{nullptr};
		bool			 reply{false}; // the sender does not know this engine yet: it is told right away
	};

	explicit DiscoveryLogic(LocalIdentity identity) : mIdentity(std::move(identity)) {}

	// The address the engine is bound to. Without a netmask, beacons from everywhere are taken.
	void				  setInterface(const net::IPv4Address &ip, const net::IPv4Address &mask);

	// Forgets every peer. Returns who was known.
	std::vector<PeerId>	  clear();

	void				  setAnnouncing(const bool announcing) { mAnnouncing = announcing; }
	bool				  isAnnouncing() const { return mAnnouncing; }

	Seen				  onBeacon(const net::SocketAddress &from, std::span<const uint8_t> datagram, TimePoint now);

	// Forgets the peers that did not announce themselves for too long, except those a session exists with
	std::vector<PeerId>	  expire(TimePoint now, const std::function<bool(PeerId)> &hasSession);

	// Whether an address is one this engine listens to
	bool				  isInSubnet(const net::IPv4Address &address) const;

	// Where announcements go: the broadcast address of the subnet
	net::IPv4Address	  broadcastAddress() const;

	std::vector<uint8_t>  beacon(bool reply) const;

	const KnownPeer		 *find(PeerId id) const;
	std::vector<PeerInfo> peers() const;
	size_t				  size() const { return mPeers.size(); }

	const LocalIdentity	 &identity() const { return mIdentity; }

private:
	struct Entry
	{
		KnownPeer peer;
		TimePoint lastSeen;
	};

	LocalIdentity			mIdentity;
	net::IPv4Address		mLocalIp;
	net::IPv4Address		mMask;
	bool					mAnnouncing{false};
	std::map<PeerId, Entry> mPeers;
};

} // namespace netlink::discovery
