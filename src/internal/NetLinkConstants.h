/*
  ==============================================================================
	Module:         NetLinkConstants
	Description:    Constants defined for the NetLink lib
  ==============================================================================
*/

#pragma once

#include <chrono>
#include <cstddef>

namespace netlink::internal
{

inline constexpr size_t					   PackageBufferSize  = 65536; // 64 KB receive buffer / max. datagram size
inline constexpr const char				  *DefaultSecret	  = "NETLINK";

// Upper bound for a single framed message
inline constexpr size_t					   MaxMessagePayload  = size_t{16} * 1024 * 1024; // 16 MiB

// Transport timings
inline constexpr std::chrono::milliseconds TcpConnectTimeout  = std::chrono::seconds{10};
inline constexpr std::chrono::milliseconds TcpSendTimeout	  = std::chrono::seconds{5};

// Maximum time a worker thread blocks in a socket wait
inline constexpr std::chrono::milliseconds SocketPollInterval = std::chrono::milliseconds{100};

} // namespace netlink::internal
