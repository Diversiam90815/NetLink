/*
==============================================================================
	Module:         MessageTypes
	Description:    Message type to send/receive messages
  ==============================================================================
*/

#pragma once

#include <cstdint>
#include <vector>

#include "NetLink/NetLink.h"

namespace netlink
{

// Internal wire-level message
struct InternalMessage
{
	uint32_t			 type{0};
	std::vector<uint8_t> data{};
};


// Message waiting in the outgoing queue together with its requested delivery guarantee
struct OutgoingMessage
{
	InternalMessage message{};
	DeliveryMode	mode{DeliveryMode::ReliableOrdered};
};

} // namespace netlink
