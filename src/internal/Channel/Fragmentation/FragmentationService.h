/*
  ==============================================================================
	Module:         FragmentationService
	Description:    Splits messages that exceed one datagram into fragments and
					reassembles them per peer
  ==============================================================================
*/

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "Channel/Protocol/PacketHeader.h"
#include "NetLinkConstants.h"
#include "Socket/SocketTypes.h"


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


struct ReassembledMessage
{
	ChannelId			 channel{ChannelId::Control};
	std::vector<uint8_t> body;
};


class FragmentationService
{
public:
	static constexpr size_t MaxFragments			  = UINT16_MAX;

	// Partially received messages kept per peer before the oldest is abandoned
	static constexpr size_t MaxPartialMessagesPerPeer = 64;

	explicit FragmentationService(const size_t maxMessageSize = internal::MaxMessagePayload) : mMaxMessageSize(maxMessageSize) {}

	// --- Splitting -------------------------------------------------

	// Number of fragments a body needs (an empty body still is one packet), 0 if it cannot be fragmented
	static size_t					  fragmentCount(size_t bodySize, size_t maxFragmentBody);

	// The index-th fragment of body. Precondition: index < fragmentCount(body.size(), maxFragmentBody)
	static Fragment					  fragmentAt(std::span<const uint8_t> body, size_t index, size_t maxFragmentBody);

	static std::vector<Fragment>	  split(std::span<const uint8_t> body, size_t maxFragmentBody);

	// --- Reassembly (per peer) -------------------------------------------------

	// Feeds one received Data packet. Returns the whole message once complete; unfragmented packets are returned immediately.
	std::optional<ReassembledMessage> accept(const net::SocketAddress &peer, const PacketHeader &header, std::span<const uint8_t> body);

	// Forgets everything partially received from the peer (restart, link reset)
	void							  reset(const net::SocketAddress &peer);
	void							  clear();

	size_t							  partialMessageCount(const net::SocketAddress &peer) const;
	size_t							  maxMessageSize() const { return mMaxMessageSize; }

private:
	struct Partial
	{
		ChannelId						  channel{ChannelId::Control};
		uint16_t						  fragCount{0};
		uint16_t						  received{0};
		size_t							  totalSize{0};
		std::vector<std::vector<uint8_t>> parts;
		std::vector<bool>				  present;
	};

	using PartialMap = std::map<uint64_t, Partial>; // key: seq of fragment 0

	size_t									 mMaxMessageSize;
	std::map<net::SocketAddress, PartialMap> mPartials;
};

} // namespace netlink::channel
