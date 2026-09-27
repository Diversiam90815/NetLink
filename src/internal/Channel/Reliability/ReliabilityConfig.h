/*
  ==============================================================================
	Module:         ReliabilityConfig
	Description:    Tunables of the reliable datagram channel.
					Injectable so tests can run with short timers.
  ==============================================================================
*/

#pragma once

#include <chrono>

#include "NetLink/NetLink.h"
#include "NetLinkConstants.h"


namespace netlink::channel
{

// Maximum number of unacknowledged fragments per link and direction (also the receive reorder window)
inline constexpr size_t WindowSize = 256;


struct ReliabilityConfig
{
	// Retransmission timeout (RFC 6298)
	std::chrono::milliseconds initialRto{100};
	std::chrono::milliseconds minRto{20};
	std::chrono::milliseconds maxRto{1000};

	// Whole messages waiting for room in the send window
	size_t					  sendQueueCapacity{1024};
	OverflowPolicy			  sendQueueOverflow{OverflowPolicy::DropNewest};

	int						  maxRetransmits{8}; // Data retransmissions before the link is considered failed
	int						  maxAckRetransmits{5};	// DataAck retransmissions while waiting for the AckAck
	size_t					  maxDatagramSize{internal::MaxDatagramSize};	// Largest datagram put on the wire, header included
	size_t					  maxMessageSize{internal::MaxMessagePayload};	// Largest reassembled message
};

} // namespace netlink::channel
