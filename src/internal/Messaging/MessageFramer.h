/*
==============================================================================
	Module:         MessageFramer
	Description:    Wire framing for InternalMessage over a byte stream
  ==============================================================================
*/

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "MessageTypes.h"
#include "NetLinkConstants.h"


namespace netlink
{

// Frame layout (network byte order): [uint32 type][uint32 payload length][payload bytes]
class MessageFramer
{
public:
	static constexpr size_t		HeaderSize = 2 * sizeof(uint32_t);

	// Precondition: message.data.size() <= internal::MaxMessagePayload
	static std::vector<uint8_t> serialize(const InternalMessage &message)
	{
		std::vector<uint8_t> frame(HeaderSize + message.data.size());

		writeUint32(frame.data(), message.type);
		writeUint32(frame.data() + sizeof(uint32_t), static_cast<uint32_t>(message.data.size()));

		if (!message.data.empty())
			std::copy(message.data.begin(), message.data.end(), frame.begin() + HeaderSize);

		return frame;
	}

	static void writeUint32(uint8_t *out, uint32_t value)
	{
		out[0] = static_cast<uint8_t>(value >> 24);
		out[1] = static_cast<uint8_t>(value >> 16);
		out[2] = static_cast<uint8_t>(value >> 8);
		out[3] = static_cast<uint8_t>(value);
	}

	static uint32_t readUint32(const uint8_t *in)
	{
		return (static_cast<uint32_t>(in[0]) << 24) | (static_cast<uint32_t>(in[1]) << 16) | (static_cast<uint32_t>(in[2]) << 8) | static_cast<uint32_t>(in[3]);
	}
};


// Incrementally reassembles frames from arbitrarily split stream chunks.
class FrameDecoder
{
public:
	explicit FrameDecoder(size_t maxPayload = internal::MaxMessagePayload) : mMaxPayload(maxPayload) {}

	void feed(std::span<const uint8_t> bytes)
	{
		if (mError || bytes.empty())
			return;

		compact();
		mBuffer.insert(mBuffer.end(), bytes.begin(), bytes.end());
	}

	// Returns the next complete message, or nullopt if more bytes are needed or the stream is corrupt
	std::optional<InternalMessage> next()
	{
		if (mError || available() < MessageFramer::HeaderSize)
			return std::nullopt;

		const uint8_t *header = mBuffer.data() + mReadOffset;
		const uint32_t type	  = MessageFramer::readUint32(header);
		const uint32_t length = MessageFramer::readUint32(header + sizeof(uint32_t));

		if (length > mMaxPayload)
		{
			mError = true;
			return std::nullopt;
		}

		if (available() < MessageFramer::HeaderSize + length)
			return std::nullopt;

		const auto		payloadBegin = mBuffer.begin() + static_cast<std::ptrdiff_t>(mReadOffset + MessageFramer::HeaderSize);

		InternalMessage message;
		message.type = type;
		message.data.assign(payloadBegin, payloadBegin + length);

		mReadOffset += MessageFramer::HeaderSize + length;
		return message;
	}

	bool   hasError() const { return mError; }
	size_t bufferedBytes() const { return available(); }

private:
	size_t available() const { return mBuffer.size() - mReadOffset; }

	// Drops consumed bytes once they make up the larger part of the buffer
	void   compact()
	{
		if (mReadOffset == 0)
			return;

		if (mReadOffset == mBuffer.size())
		{
			mBuffer.clear();
			mReadOffset = 0;
		}
		else if (mReadOffset * 2 >= mBuffer.size())
		{
			mBuffer.erase(mBuffer.begin(), mBuffer.begin() + static_cast<std::ptrdiff_t>(mReadOffset));
			mReadOffset = 0;
		}
	}

	std::vector<uint8_t> mBuffer;
	size_t				 mReadOffset{0};
	size_t				 mMaxPayload;
	bool				 mError{false};
};

} // namespace netlink
