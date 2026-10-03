/*
  ==============================================================================
	Module:         MessageAssembler
	Description:    Puts the fragments of one stream back together into messages
  ==============================================================================
*/

#include "MessageAssembler.h"

#include <algorithm>

#include "NetLinkLog.h"


namespace netlink::channel
{

std::optional<AssembledMessage> MessageAssembler::accept(const PacketHeader &header, const std::span<const uint8_t> body)
{
	if (!header.flags.isFragmented())
	{
		reset();

		if (body.size() > mMaxMessageSize)
			return std::nullopt;

		return AssembledMessage{.tag = header.tag, .body = std::vector<uint8_t>(body.begin(), body.end())};
	}

	if (header.fragIndex == 0)
	{
		reset();

		mAssembling = true;
		mFragCount	= header.fragCount;
		mTag		= header.tag;

		// Every fragment but the last one is as large as the first
		mBody.reserve(std::min(body.size() * header.fragCount, mMaxMessageSize));
	}

	if (!mAssembling || header.fragCount != mFragCount || header.fragIndex != mNextIndex)
	{
		NETLINK_LOG_WARNING("Fragment {} of {} does not continue the message in progress, dropping the message", header.fragIndex, header.fragCount);
		reset();
		return std::nullopt;
	}

	if (mBody.size() + body.size() > mMaxMessageSize)
	{
		NETLINK_LOG_WARNING("Message exceeds {} bytes, dropping it", mMaxMessageSize);
		reset();
		return std::nullopt;
	}

	mBody.insert(mBody.end(), body.begin(), body.end());
	++mNextIndex;

	if (mNextIndex < mFragCount)
		return std::nullopt;

	AssembledMessage message{.tag = mTag, .body = std::move(mBody)};
	reset();
	return message;
}


void MessageAssembler::reset()
{
	mAssembling = false;
	mFragCount	= 0;
	mNextIndex	= 0;
	mTag		= 0;
	mBody		= {};
}

} // namespace netlink::channel
