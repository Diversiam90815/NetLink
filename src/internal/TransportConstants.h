/*
  ==============================================================================
	Module:         TransportConstants
	Description:    The fixed numbers of the transport, and the few timings
					tests may shorten
  ==============================================================================
*/

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>


namespace netlink::internal
{

inline constexpr size_t PackageBufferSize		 = 65536; // 64 KB receive buffer / max. datagram size

// Upper bound for a single (reassembled) message
inline constexpr size_t MaxMessagePayload		 = size_t{16} * 1024 * 1024; // 16 MiB

// ... and for one that is sent without acknowledgement
inline constexpr size_t MaxMediaPayload			 = size_t{64} * 1024;

// Largest datagram an engine puts on the wire: stays below the Ethernet MTU, so IP never fragments it
inline constexpr size_t MaxDatagramSize			 = 1200;

// Receive buffer of the channel socket: large enough to absorb bursts from many peers at once (OS defaults can be as
// small as 64 KB, about 50 datagrams). The OS may cap it.
inline constexpr int	ChannelReceiveBufferSize = 4 * 1024 * 1024;

// Send buffer of the channel socket, per datagram of one burst of its send budget (what Linux accounts for a full
// datagram). Windows keeps its default: a larger buffer measurably slowed sending there.
#if defined(_WIN32)
inline constexpr int ChannelSendBufferPerDatagram = 0;
#else
inline constexpr int ChannelSendBufferPerDatagram = 2304;
#endif

// How often an engine announces itself and looks at its network adapter
inline constexpr auto	BeaconInterval	   = std::chrono::seconds{2};

// A discovered peer that did not announce itself for this long is forgotten
inline constexpr auto	PeerExpiry		   = std::chrono::seconds{6};
inline constexpr size_t MaxDiscoveredPeers = 1024;

// How long the application may take to answer a connection request
inline constexpr auto	DecisionTimeout	   = std::chrono::seconds{30};

// A session that is being ended waits this long for what is still to be sent, then for its goodbye to be acknowledged
inline constexpr auto	DrainTimeout	   = std::chrono::seconds{1};
inline constexpr auto	CloseLinger		   = std::chrono::milliseconds{250};

// Sessions that are not connected yet: requests of further peers are ignored
inline constexpr size_t MaxPendingSessions = 64;

} // namespace netlink::internal


namespace netlink::channel
{

// Unacknowledged packets per lane and direction, also the receive reorder window. Powers of two.
inline constexpr size_t	  ControlWindow			  = 32;
inline constexpr size_t	  ReliableWindow		  = 256;
inline constexpr size_t	  BulkWindow			  = 256;

// Reliable and Bulk packets that may be unacknowledged at once, adapted to the path
inline constexpr size_t	  MinCongestionWindow	  = 8;
inline constexpr size_t	  MaxCongestionWindow	  = 256;
inline constexpr size_t	  InitialCongestionWindow = 32;

// A packet counts as lost once this many packets that were sent after it are acknowledged
inline constexpr size_t	  FastRetransmitThreshold = 3;

// How often a sender asks a lane again that the receiver paused: no faster than this, and backing off to the largest timeout
inline constexpr auto	  MinPauseProbeInterval	  = std::chrono::milliseconds{50};

// Unacknowledged (media) messages: waiting to be sent, and being put together at once
inline constexpr size_t	  MediaQueueMessages	  = 32;
inline constexpr size_t	  MediaAssemblySlots	  = 4;

// Delivered payload that waits for the application's callback: senders are paused above, and resumed below
inline constexpr size_t	  BacklogPauseBytes		  = size_t{32} * 1024 * 1024;
inline constexpr size_t	  BacklogResumeBytes	  = size_t{16} * 1024 * 1024;

// Messages that are being put together, all peers of an engine together
inline constexpr size_t	  AssemblyBudgetBytes	  = size_t{256} * 1024 * 1024;

// Messages of one lane to one peer that wait to be sent
inline constexpr size_t	  DefaultSendQueueBytes	  = size_t{64} * 1024 * 1024;

// Datagrams an engine sends per second, to all peers together
inline constexpr uint32_t DefaultMaxSendRate	  = 80'000;


// What tests shorten to run in test time. Production code uses the defaults.
struct LinkTimings
{
	// Retransmission timeout (RFC 6298)
	std::chrono::milliseconds initialRto{100};
	std::chrono::milliseconds minRto{20};
	std::chrono::milliseconds maxRto{1000};

	// Nothing arrived from the peer, or data in flight made no progress, for this long: the peer is lost
	std::chrono::milliseconds peerTimeout{5000};

	// Nothing arrived for this long: the peer is asked whether it is still there
	std::chrono::milliseconds keepAlive{1000};

	size_t					  initialCwnd{InitialCongestionWindow};
	size_t					  fixedCwnd{0}; // not 0: the congestion window is this, whatever happens
};

} // namespace netlink::channel
