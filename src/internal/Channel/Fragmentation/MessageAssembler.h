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
#include "TransportConstants.h"


namespace netlink::channel
{

struct AssembledMessage
{
	uint32_t			 tag{0};
	std::vector<uint8_t> body;
};


// The memory messages may take while they are being put together, shared by every stream that is given the same budget
class AssemblyBudget
{
public:
	explicit AssemblyBudget(const size_t limit = AssemblyBudgetBytes) : mLimit(limit) {}

	bool fits(const size_t bytes) const { return bytes <= mLimit - mUsed; }

	bool reserve(const size_t bytes)
	{
		if (!fits(bytes))
			return false;

		mUsed += bytes;
		return true;
	}

	void   release(const size_t bytes) { mUsed -= bytes; }

	size_t used() const { return mUsed; }

private:
	size_t mLimit;
	size_t mUsed{0};
};


class MessageAssembler
{
public:
	// Without a budget every message is taken
	explicit MessageAssembler(AssemblyBudget *budget = nullptr) : mBudget(budget) {}
	~MessageAssembler() { reset(); }

	MessageAssembler(const MessageAssembler &)							= delete;
	MessageAssembler			   &operator=(const MessageAssembler &) = delete;

	// Whether the packet can be taken right now. Only the first fragment of a message is ever refused: when the budget
	// has no room for the whole message.
	bool							hasRoomFor(const PacketHeader &header) const { return !mBudget || !header.startsFragmentedMessage() || mBudget->fits(header.totalLength); }

	// Feeds the next Data packet of the stream. Returns the whole message once it is complete; unfragmented packets are returned immediately.
	std::optional<AssembledMessage> accept(const PacketHeader &header, std::span<const uint8_t> body);

	// Forgets the message in progress (stream reset)
	void							reset();

	bool							isAssembling() const { return mAssembling; }

private:
	AssemblyBudget		*mBudget;

	bool				 mAssembling{false};
	uint16_t			 mFragCount{0};
	uint16_t			 mNextIndex{0};
	uint32_t			 mTag{0};
	uint32_t			 mTotalLength{0}; // as the first fragment announced it, and what is reserved in the budget
	std::vector<uint8_t> mBody;
};

} // namespace netlink::channel
