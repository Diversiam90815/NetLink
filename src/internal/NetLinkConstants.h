/*
  ==============================================================================
	Module:         NetLinkConstants
	Description:    Constants defined for the NetLink lib
  ==============================================================================
*/

#pragma once

#include <chrono>

namespace netlink::internal
{

inline constexpr size_t PackageBufferSize  = 65536; // 64 KB receive buffer / max. datagram size

// Upper bound for a single (reassembled) message
inline constexpr size_t MaxMessagePayload  = size_t{16} * 1024 * 1024; // 16 MiB

// Largest datagram the peer channel puts on the wire: stays below the Ethernet MTU, so IP never fragments it
inline constexpr size_t MaxDatagramSize	   = 1200;

// Maximum time a worker thread blocks in a socket wait
inline constexpr auto	SocketPollInterval = std::chrono::milliseconds{100};

} // namespace netlink::internal
