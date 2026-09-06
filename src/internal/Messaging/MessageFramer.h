/*
==============================================================================
	Module:         MessageFramer
	Description:    Wire framing for InternalMessage over a TCP byte stream
  ==============================================================================
*/

#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#include "MessageTypes.h"


namespace netlink
{

// Frame layout: [uint32_t type][uint32_t length][payload bytes]
class MessageFramer
{
public:
	static constexpr size_t		headerSize = sizeof(uint32_t) * 2;

	static std::vector<uint8_t> serialize(const InternalMessage &message)
	{
		std::vector<uint8_t> frame(headerSize + message.data.size());

		uint32_t			 type = message.type;
		uint32_t			 len  = static_cast<uint32_t>(message.data.size());

		std::memcpy(frame.data(), &type, sizeof(type));
		std::memcpy(frame.data() + sizeof(type), &len, sizeof(len));

		if (!message.data.empty())
			std::memcpy(frame.data() + headerSize, message.data.data(), message.data.size());

		return frame;
	}

	// Appends newBytes to the accumulator, extracts as many complete messages as available.
	// Leaves any trailing partial frame in the accumulator for next call.
	static std::vector<InternalMessage> extractMessages(std::vector<uint8_t> &accumulator)
	{
		std::vector<InternalMessage> messages;
		size_t						 offset = 0;

		while (accumulator.size() - offset >= headerSize)
		{
			uint32_t type = 0;
			uint32_t len  = 0;
			std::memcpy(&type, accumulator.data() + offset, sizeof(type));
			std::memcpy(&len, accumulator.data() + offset + sizeof(type), sizeof(len));

			if (accumulator.size() - offset < headerSize + len)
				break; // incomplete payload, wait for more bytes

			InternalMessage msg;
			msg.type = type;
			msg.data.assign(accumulator.begin() + offset + headerSize, accumulator.begin() + offset + headerSize + len);
			messages.push_back(std::move(msg));

			offset += headerSize + len;
		}

		if (offset > 0)
			accumulator.erase(accumulator.begin(), accumulator.begin() + offset);

		return messages;
	}
};

} // namespace netlink
