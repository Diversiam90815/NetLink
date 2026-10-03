/*
  ==============================================================================
	Module:         MessageAssembler
	Description:    Puts the fragments of one stream back together into messages
  ==============================================================================
*/

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "Channel/Protocol/PacketHeader.h"
#include "NetLinkConstants.h"


namespace netlink::channel
{

struct AssembledMessage
{
	uint32_t			 tag{0};
	std::vector<uint8_t> body;
};


class MessageAssembler
{
public:
	// Memory set aside when a message starts. The first fragment only claims how large its message is: more is
	// allocated as the fragments really arrive.
	static constexpr size_t MaxInitialReserve = size_t{1024} * 1024;

	// fragmentBody: the size of every fragment but the last one of a message, as the sending link cuts them
	explicit MessageAssembler(const size_t maxMessageSize = internal::MaxMessagePayload, const size_t fragmentBody = MaxFragmentBody)
		: mMaxMessageSize(maxMessageSize), mFragmentBody(fragmentBody)
	{
	}

	// Feeds the next Data packet of the stream. Returns the whole message once it is complete; unfragmented packets are returned immediately.
	std::optional<AssembledMessage> accept(const PacketHeader &header, std::span<const uint8_t> body);

	// Forgets the message in progress (stream reset)
	void							reset();

	bool							isAssembling() const { return mAssembling; }
	size_t							maxMessageSize() const { return mMaxMessageSize; }

	// Bytes currently allocated for the message in progress
	size_t							reservedBytes() const { return mBody.capacity(); }

private:
	size_t				 mMaxMessageSize;
	size_t				 mFragmentBody;

	bool				 mAssembling{false};
	uint16_t			 mFragCount{0};
	uint16_t			 mNextIndex{0};
	uint32_t			 mTag{0};
	std::vector<uint8_t> mBody;
};

} // namespace netlink::channel
