/*
  ==============================================================================
	Module:         FragmentationService
	Description:    Splits messages that exceed one datagram into fragments.
					The receiving side puts them together with a MessageAssembler.
  ==============================================================================
*/

#pragma once

#include <cstdint>
#include <span>
#include <vector>


namespace netlink::channel
{

struct Fragment
{
	uint16_t				 index{0};
	uint16_t				 count{1};
	std::span<const uint8_t> body;

	bool					 isFragmented() const { return count > 1; }
	bool					 isLast() const { return index + 1 == count; }
};


class FragmentationService
{
public:
	static constexpr size_t		  MaxFragments = UINT16_MAX;

	// Number of fragments a body needs (an empty body still is one packet), 0 if it cannot be fragmented
	static size_t				  fragmentCount(size_t bodySize, size_t maxFragmentBody);

	// The index-th fragment of body. Precondition: index < fragmentCount(body.size(), maxFragmentBody)
	static Fragment				  fragmentAt(std::span<const uint8_t> body, size_t index, size_t maxFragmentBody);

	static std::vector<Fragment> split(std::span<const uint8_t> body, size_t maxFragmentBody);
};

} // namespace netlink::channel
