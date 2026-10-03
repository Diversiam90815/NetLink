/*
  ==============================================================================
	Module:         FragmentationService
	Description:    Splits messages that exceed one datagram into fragments.
					The receiving side puts them together with a MessageAssembler.
  ==============================================================================
*/

#include "FragmentationService.h"

#include <algorithm>


namespace netlink::channel
{

size_t FragmentationService::fragmentCount(const size_t bodySize, const size_t maxFragmentBody)
{
	if (maxFragmentBody == 0)
		return 0;

	const size_t count = bodySize == 0 ? 1 : (bodySize + maxFragmentBody - 1) / maxFragmentBody;
	return count <= MaxFragments ? count : 0;
}


Fragment FragmentationService::fragmentAt(const std::span<const uint8_t> body, const size_t index, const size_t maxFragmentBody)
{
	const size_t count	= fragmentCount(body.size(), maxFragmentBody);
	const size_t offset = std::min(index * maxFragmentBody, body.size());
	const size_t length = std::min(maxFragmentBody, body.size() - offset);

	return {.index = static_cast<uint16_t>(index), .count = static_cast<uint16_t>(count), .body = body.subspan(offset, length)};
}


std::vector<Fragment> FragmentationService::split(const std::span<const uint8_t> body, const size_t maxFragmentBody)
{
	std::vector<Fragment> fragments;
	const size_t		  count = fragmentCount(body.size(), maxFragmentBody);

	fragments.reserve(count);
	for (size_t i = 0; i < count; ++i)
		fragments.push_back(fragmentAt(body, i, maxFragmentBody));

	return fragments;
}

} // namespace netlink::channel
