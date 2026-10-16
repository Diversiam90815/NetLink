/*
==============================================================================
	Module:         NetLinkC
	Description:    Plain C API of the NetLink library
  ==============================================================================
*/

#include "NetLink/NetLinkC.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <new>
#include <span>
#include <string>
#include <string_view>

#include "NetLink/NetLink.h"
#include "NetLinkVersion.h"


// The C enums are passed on as they are
static_assert(NL_LANE_MEDIA == static_cast<int>(netlink::Lane::Media));
static_assert(NL_SEND_NOT_RUNNING == static_cast<int>(netlink::SendResult::NotRunning));
static_assert(NL_DISCONNECT_SHUTDOWN == static_cast<int>(netlink::DisconnectReason::Shutdown));
static_assert(NL_ADAPTER_PREFERRED == static_cast<int>(netlink::AdapterPriority::Preferred));
static_assert(NL_LOG_ERROR == static_cast<int>(netlink::LogLevel::Error));


struct NLNetLink
{
	netlink::NetLink instance;
};


namespace
{

void copyString(char (&target)[NL_MAX_STRING], const std::string_view source)
{
	size_t length = std::min(source.size(), static_cast<size_t>(NL_MAX_STRING - 1));

	// A text that is cut off must still be valid UTF-8: never keep the first bytes of a character only
	if (length < source.size())
	{
		while (length > 0 && (static_cast<unsigned char>(source[length]) & 0xC0) == 0x80)
			--length;
	}

	std::memcpy(target, source.data(), length);
	target[length] = '\0';
}


NLPeerInfo toC(const netlink::PeerInfo &peer)
{
	NLPeerInfo result{};
	result.id	= peer.id.value;
	result.port = peer.port;
	copyString(result.displayName, peer.displayName);
	copyString(result.address, peer.address);
	copyString(result.appVersion, peer.appVersion);
	return result;
}


NLNetworkAdapter toC(const netlink::NetworkAdapter &adapter)
{
	NLNetworkAdapter result{};
	result.id		= adapter.id;
	result.priority = static_cast<int32_t>(adapter.priority);
	copyString(result.adapterName, adapter.adapterName);
	copyString(result.networkName, adapter.networkName);
	copyString(result.ipv4, adapter.ipv4);
	return result;
}


bool isLane(const int32_t lane)
{
	return lane >= NL_LANE_RELIABLE && lane <= NL_LANE_MEDIA;
}


// Copies what fits and reports how many there are
template <typename Source, typename Target, typename Convert>
int32_t copyOut(const Source &source, Target *target, const int32_t maxCount, Convert convert)
{
	if (target && maxCount > 0)
	{
		const size_t count = std::min(source.size(), static_cast<size_t>(maxCount));

		for (size_t i = 0; i < count; ++i)
			target[i] = convert(source[i]);
	}

	return static_cast<int32_t>(source.size());
}


netlink::NetLinkCallbacks toCpp(const NLCallbacks &c)
{
	netlink::NetLinkCallbacks callbacks;
	void					 *context = c.context;

	if (c.onPeerDiscovered)
		callbacks.onPeerDiscovered = [fn = c.onPeerDiscovered, context](const netlink::PeerInfo &peer)
		{
			const NLPeerInfo info = toC(peer);
			fn(context, &info);
		};

	if (c.onPeerLost)
		callbacks.onPeerLost = [fn = c.onPeerLost, context](const netlink::PeerId peer) { fn(context, peer.value); };

	// Left unset, NetLink accepts every request on its own
	if (c.onConnectionRequest)
		callbacks.onConnectionRequest = [fn = c.onConnectionRequest, context](const netlink::PeerInfo &peer)
		{
			const NLPeerInfo info = toC(peer);
			fn(context, &info);
		};

	if (c.onConnected)
		callbacks.onConnected = [fn = c.onConnected, context](const netlink::PeerInfo &peer)
		{
			const NLPeerInfo info = toC(peer);
			fn(context, &info);
		};

	if (c.onDisconnected)
		callbacks.onDisconnected = [fn = c.onDisconnected, context](const netlink::PeerId peer, const netlink::DisconnectReason reason)
		{ fn(context, peer.value, static_cast<int32_t>(reason)); };

	if (c.onMessage)
		callbacks.onMessage = [fn = c.onMessage, context](const netlink::PeerId from, const netlink::Lane lane, netlink::Message &&message)
		{ fn(context, from.value, static_cast<int32_t>(lane), message.type, message.data.data(), message.data.size()); };

	if (c.onNetworkAdapterChanged)
		callbacks.onNetworkAdapterChanged = [fn = c.onNetworkAdapterChanged, context](const netlink::NetworkAdapter &adapter)
		{
			const NLNetworkAdapter info = toC(adapter);
			fn(context, &info);
		};

	if (c.onLog)
		callbacks.onLog = [fn = c.onLog, context](const netlink::LogLevel level, const std::string_view message)
		{
			// The view is not terminated
			const std::string text(message);
			fn(context, static_cast<int32_t>(level), text.c_str());
		};

	return callbacks;
}

} // namespace


// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

const char *nl_version(void)
{
	return netlink::internal::Version;
}


NLNetLink *nl_create(void)
{
	return new (std::nothrow) NLNetLink();
}


void nl_destroy(NLNetLink *netlink)
{
	delete netlink;
}


bool nl_start(NLNetLink *netlink, const NLConfig *config, const NLCallbacks *callbacks)
{
	if (!netlink || !config || !config->appId)
		return false;

	netlink::NetLinkConfig cppConfig;
	cppConfig.displayName = config->displayName ? config->displayName : "";
	cppConfig.appId		  = config->appId;
	cppConfig.appVersion  = config->appVersion ? config->appVersion : "";

	if (config->discoveryPort != 0)
		cppConfig.discoveryPort = config->discoveryPort;

	if (config->sendQueueBytes != 0)
		cppConfig.sendQueueBytes = static_cast<size_t>(config->sendQueueBytes);

	if (config->maxSendRate != 0)
		cppConfig.maxSendRate = config->maxSendRate;

	if (config->peerTimeoutMs != 0)
		cppConfig.peerTimeout = std::chrono::milliseconds(config->peerTimeoutMs);

	return netlink->instance.start(cppConfig, callbacks ? toCpp(*callbacks) : netlink::NetLinkCallbacks{});
}


void nl_stop(NLNetLink *netlink)
{
	if (netlink)
		netlink->instance.stop();
}


// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

bool nl_start_discovery(NLNetLink *netlink)
{
	return netlink && netlink->instance.startDiscovery();
}


void nl_stop_discovery(NLNetLink *netlink)
{
	if (netlink)
		netlink->instance.stopDiscovery();
}


int32_t nl_get_peers(const NLNetLink *netlink, NLPeerInfo *peers, const int32_t maxPeers)
{
	if (!netlink)
		return 0;

	return copyOut(netlink->instance.peers(), peers, maxPeers, [](const netlink::PeerInfo &peer) { return toC(peer); });
}


// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------

bool nl_connect(NLNetLink *netlink, const uint64_t peer)
{
	return netlink && netlink->instance.connect(netlink::PeerId{peer});
}


void nl_accept(NLNetLink *netlink, const uint64_t peer)
{
	if (netlink)
		netlink->instance.accept(netlink::PeerId{peer});
}


void nl_decline(NLNetLink *netlink, const uint64_t peer)
{
	if (netlink)
		netlink->instance.decline(netlink::PeerId{peer});
}


void nl_disconnect(NLNetLink *netlink, const uint64_t peer)
{
	if (netlink)
		netlink->instance.disconnect(netlink::PeerId{peer});
}


int32_t nl_get_connected_peers(const NLNetLink *netlink, uint64_t *peers, const int32_t maxPeers)
{
	if (!netlink)
		return 0;

	return copyOut(netlink->instance.connectedPeers(), peers, maxPeers, [](const netlink::PeerId peer) { return peer.value; });
}


// ---------------------------------------------------------------------------
// Messaging
// ---------------------------------------------------------------------------

int32_t nl_send(NLNetLink *netlink, const uint64_t peer, const uint32_t type, const uint8_t *data, const size_t size, const int32_t lane, const uint32_t timeoutMs)
{
	if (!netlink || (!data && size > 0) || !isLane(lane))
		return NL_SEND_INVALID;

	const auto result = netlink->instance.send(netlink::PeerId{peer}, type, std::span<const uint8_t>(data, size), static_cast<netlink::Lane>(lane),
											   std::chrono::milliseconds(timeoutMs));
	return static_cast<int32_t>(result);
}


size_t nl_broadcast(NLNetLink *netlink, const uint32_t type, const uint8_t *data, const size_t size, const int32_t lane)
{
	if (!netlink || (!data && size > 0) || !isLane(lane))
		return 0;

	return netlink->instance.broadcast(type, std::span<const uint8_t>(data, size), static_cast<netlink::Lane>(lane));
}


bool nl_get_stats(const NLNetLink *netlink, const uint64_t peer, NLPeerStats *stats)
{
	if (!netlink || !stats)
		return false;

	const auto found = netlink->instance.stats(netlink::PeerId{peer});
	if (!found)
		return false;

	stats->rttMicroseconds = static_cast<uint64_t>(found->rtt.count());
	stats->bytesQueued	   = found->bytesQueued;
	stats->bytesSent	   = found->bytesSent;
	stats->bytesReceived   = found->bytesReceived;
	stats->retransmissions = found->retransmissions;
	stats->mediaSent	   = found->mediaSent;
	stats->mediaDropped	   = found->mediaDropped;
	stats->mediaIncomplete = found->mediaIncomplete;
	stats->mediaLoss	   = found->mediaLoss;
	return true;
}


// ---------------------------------------------------------------------------
// Network adapters
// ---------------------------------------------------------------------------

int32_t nl_get_adapters(const NLNetLink *netlink, NLNetworkAdapter *adapters, const int32_t maxAdapters)
{
	if (!netlink)
		return 0;

	return copyOut(netlink->instance.getAvailableAdapters(), adapters, maxAdapters, [](const netlink::NetworkAdapter &adapter) { return toC(adapter); });
}


bool nl_set_active_adapter(NLNetLink *netlink, const uint64_t adapterId)
{
	return netlink && netlink->instance.setActiveAdapter(adapterId);
}


uint64_t nl_get_active_adapter_id(const NLNetLink *netlink)
{
	return netlink ? netlink->instance.getActiveAdapterID() : 0;
}
