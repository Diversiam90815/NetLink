/*
  ==============================================================================
	Module:         ReliabilityConfig
	Description:    Tunables of the reliable datagram channel.
  ==============================================================================
*/

#pragma once

#include <chrono>

#include "NetLink/NetLink.h"
#include "NetLinkConstants.h"


namespace netlink::channel
{

// Maximum number of unacknowledged fragments per link, channel and direction (also the receive reorder window)
inline constexpr size_t WindowSize = 1024;


struct ReliabilityConfig
{
	// Retransmission timeout (RFC 6298)
	std::chrono::milliseconds initialRto{100};
	std::chrono::milliseconds minRto{20};
	std::chrono::milliseconds maxRto{1000};

	// Whole messages waiting to be sent
	size_t					  sendQueueCapacity{2048};
	OverflowPolicy			  sendQueueOverflow{OverflowPolicy::DropNewest};

	// Data is waiting for its acknowledgement and none at all arrived for this long
	std::chrono::milliseconds failureTimeout{5000};

	int						  maxAckRetransmits{5};						   // DataAck retransmissions while waiting for the AckAck
	size_t					  maxDatagramSize{internal::MaxDatagramSize};  // Largest datagram put on the wire, header included
	size_t					  maxMessageSize{internal::MaxMessagePayload}; // Largest reassembled message

	// Congestion window: Data packets that may be unacknowledged at the same time
	size_t					  initialCongestionWindow{16};
	size_t					  minCongestionWindow{8};
	size_t					  maxCongestionWindow{WindowSize};

	// A packet counts as lost once this many packets that were sent after it are acknowledged (fast retransmit)
	size_t					  reorderThreshold{3};

	// How often the sender asks again while the receiver's window is closed (its application is not keeping up)
	std::chrono::milliseconds windowProbeInterval{50};

	// Unreliable datagrams waiting for a send pass; when full, the oldest is dropped
	size_t					  unreliableQueueCapacity{128};
};

} // namespace netlink::channel
