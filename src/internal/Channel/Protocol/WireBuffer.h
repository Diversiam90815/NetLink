/*
  ==============================================================================
	Module:         WireBuffer
	Description:    Writes and reads the fields of a binary message one after
					another (network byte order)
  ==============================================================================
*/

#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ByteOrder.h"


namespace netlink::channel
{

class WireWriter
{
public:
	void u8(const uint8_t value) { mBytes.push_back(value); }

	void u16(const uint16_t value)
	{
		mBytes.resize(mBytes.size() + 2);
		writeUint16(mBytes.data() + mBytes.size() - 2, value);
	}

	void u64(const uint64_t value)
	{
		mBytes.resize(mBytes.size() + 8);
		writeUint64(mBytes.data() + mBytes.size() - 8, value);
	}

	// One length byte, then the text: cut off at 255 bytes
	void text(const std::string_view value)
	{
		const auto size = static_cast<uint8_t>(std::min<size_t>(value.size(), UINT8_MAX));
		u8(size);
		mBytes.insert(mBytes.end(), value.begin(), value.begin() + size);
	}

	void				 bytes(const std::span<const uint8_t> value) { mBytes.insert(mBytes.end(), value.begin(), value.end()); }

	std::vector<uint8_t> take() { return std::move(mBytes); }

private:
	std::vector<uint8_t> mBytes;
};


// Every read fails once one read went past the end
class WireReader
{
public:
	explicit WireReader(const std::span<const uint8_t> bytes) : mBytes(bytes) {}

	uint8_t u8()
	{
		const auto field = next(1);
		return field.empty() ? uint8_t{0} : field[0];
	}

	uint16_t u16()
	{
		const auto field = next(2);
		return field.empty() ? uint16_t{0} : readUint16(field.data());
	}

	uint64_t u64()
	{
		const auto field = next(8);
		return field.empty() ? uint64_t{0} : readUint64(field.data());
	}

	std::string text()
	{
		const auto field = next(u8());
		return {field.begin(), field.end()};
	}

	std::span<const uint8_t> next(const size_t size)
	{
		if (mFailed || mBytes.size() < size)
		{
			mFailed = true;
			return {};
		}

		const auto field = mBytes.first(size);
		mBytes			 = mBytes.subspan(size);
		return field;
	}

	// Everything was read and nothing is left over
	bool complete() const { return !mFailed && mBytes.empty(); }
	bool failed() const { return mFailed; }

private:
	std::span<const uint8_t> mBytes;
	bool					 mFailed{false};
};

} // namespace netlink::channel
