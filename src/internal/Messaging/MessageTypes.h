/*
==============================================================================
	Module:         MessageTypes
	Description:    Message type to send/receive messages
  ==============================================================================
*/

#pragma once

#include <cstdint>
#include <vector>

namespace netlink
{

// Internal wire-level message
struct MessageTypes
{
	uint32_t			 type{0};
	std::vector<uint8_t> data{};
};

} // namespace netlink
