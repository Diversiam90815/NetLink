/*
==============================================================================
	Module:         SocketTypes
	Description:    Portable vocabulary types of the socket layer.
  ==============================================================================
*/

#pragma once

#include <cstdint>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

#include "IPv4Address.h"


namespace netlink::net
{

// IPv4 address + port
struct SocketAddress
{
	IPv4Address			 ip{};
	uint16_t			 port{0};

	bool				 operator==(const SocketAddress &other) const = default;

	std::string			 toString() const { return ip.toString() + ":" + std::to_string(port); }

	static SocketAddress any(uint16_t port = 0) { return {IPv4Address::unspecified(), port}; }
};


enum class SocketError : uint8_t
{
	Timeout,			 // The wait expired before the operation could complete
	WouldBlock,			 // Non-blocking operation not ready
	Cancelled,			 // Aborted through a cancellation request
	Closed,				 // Orderly shutdown by the peer, or the socket was shut down locally
	ConnectionRefused,	 // Nobody listens on the remote port
	ConnectionReset,	 // Peer reset the connection
	ConnectionAborted,	 // Connection aborted by the local stack
	NotConnected,		 // Stream operation on an unconnected socket
	AddressInUse,		 // Local address/port already bound
	AddressNotAvailable, // Local address does not belong to this machine
	PermissionDenied,	 // e.g. broadcast without SO_BROADCAST, privileged port
	InvalidArgument,	 // Malformed address or invalid socket state
	NetworkUnreachable,	 // No route to the remote host
	MessageTooLarge,	 // Datagram larger than the buffer / MTU
	NotInitialized,		 // Socket subsystem could not be initialized
	Unknown,
};


constexpr std::string_view toString(SocketError error)
{
	switch (error)
	{
	case SocketError::Timeout: return "Timeout";
	case SocketError::WouldBlock: return "Would block";
	case SocketError::Cancelled: return "Cancelled";
	case SocketError::Closed: return "Closed";
	case SocketError::ConnectionRefused: return "Connection refused";
	case SocketError::ConnectionReset: return "Connection reset";
	case SocketError::ConnectionAborted: return "Connection aborted";
	case SocketError::NotConnected: return "Not connected";
	case SocketError::AddressInUse: return "Address in use";
	case SocketError::AddressNotAvailable: return "Address not available";
	case SocketError::PermissionDenied: return "Permission denied";
	case SocketError::InvalidArgument: return "Invalid argument";
	case SocketError::NetworkUnreachable: return "Network unreachable";
	case SocketError::MessageTooLarge: return "Message too large";
	case SocketError::NotInitialized: return "Socket subsystem not initialized";
	case SocketError::Unknown: return "Unknown error";
	}
	return "Unrecognized error";
}


template <typename T>
using Result = std::expected<T, SocketError>;


struct BindOptions
{
	bool enableBroadcast   = false;
	bool reuseAddress	   = false; // Allow several sockets on the same port (needed for LAN discovery)
	int	 receiveBufferSize = 0;		// 0 = OS default
	int	 sendBufferSize	   = 0;		// 0 = OS default
};


struct Datagram
{
	size_t		  size{0};
	SocketAddress from{};
};

} // namespace netlink::net
