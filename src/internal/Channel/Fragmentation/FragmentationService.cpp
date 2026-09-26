/*
  ==============================================================================
	Module:         FragmentationService
	Description:    Splits messages that exceed one datagram into fragments and
					reassembles them per peer
  ==============================================================================
*/

#include "FragmentationService.h"

#include <algorithm>

#include "NetLinkLog.h"


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

	return {static_cast<uint16_t>(index), static_cast<uint16_t>(count), body.subspan(offset, length)};
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


std::optional<ReassembledMessage> FragmentationService::accept(const net::SocketAddress &peer, const PacketHeader &header, std::span<const uint8_t> body)
{
	const ChannelId channel = header.flags.channel();

	if (!header.flags.isFragmented())
	{
		if (body.size() > mMaxMessageSize)
			return std::nullopt;

		return ReassembledMessage{channel, std::vector<uint8_t>(body.begin(), body.end())};
	}

	if (header.seq < header.fragIndex)
		return std::nullopt;

	const uint64_t firstSeq = header.seq - header.fragIndex;
	auto		  &partials = mPartials[peer];
	auto		   it		= partials.find(firstSeq);

	if (it == partials.end())
	{
		// Unbounded partial state would let a peer exhaust memory: abandon the oldest message
		if (partials.size() >= MaxPartialMessagesPerPeer)
			partials.erase(partials.begin());

		Partial partial;
		partial.channel	  = channel;
		partial.fragCount = header.fragCount;
		partial.parts.resize(header.fragCount);
		partial.present.resize(header.fragCount, false);
		it = partials.emplace(firstSeq, std::move(partial)).first;
	}

	Partial &partial = it->second;

	if (partial.fragCount != header.fragCount || partial.channel != channel)
	{
		NETLINK_LOG_WARNING("Fragment {} of message {} from {} is inconsistent, dropping the message", header.fragIndex, firstSeq, peer.toString());
		partials.erase(it);
		return std::nullopt;
	}

	if (partial.present[header.fragIndex])
		return std::nullopt; // duplicate fragment

	if (partial.totalSize + body.size() > mMaxMessageSize)
	{
		NETLINK_LOG_WARNING("Message {} from {} exceeds {} bytes, dropping it", firstSeq, peer.toString(), mMaxMessageSize);
		partials.erase(it);
		return std::nullopt;
	}

	partial.parts[header.fragIndex].assign(body.begin(), body.end());
	partial.present[header.fragIndex] = true;
	partial.totalSize += body.size();
	++partial.received;

	if (partial.received < partial.fragCount)
		return std::nullopt;

	ReassembledMessage message;
	message.channel = partial.channel;
	message.body.reserve(partial.totalSize);

	for (const auto &part : partial.parts)
		message.body.insert(message.body.end(), part.begin(), part.end());

	partials.erase(it);
	if (partials.empty())
		mPartials.erase(peer);

	return message;
}


void FragmentationService::reset(const net::SocketAddress &peer)
{
	mPartials.erase(peer);
}


void FragmentationService::clear()
{
	mPartials.clear();
}


size_t FragmentationService::partialMessageCount(const net::SocketAddress &peer) const
{
	const auto it = mPartials.find(peer);
	return it != mPartials.end() ? it->second.size() : 0;
}

} // namespace netlink::channel
