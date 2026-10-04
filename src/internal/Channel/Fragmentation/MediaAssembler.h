/*
  ==============================================================================
	Module:         MediaAssembler
	Description:    Puts the fragments of unacknowledged messages together:
					they arrive in any order, and some never arrive
  ==============================================================================
*/

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "Channel/Protocol/PacketHeader.h"
#include "MessageAssembler.h"


namespace netlink::channel
{

/*
 A message is identified by the seq of its first datagram (seq - fragIndex). A few messages are put together at the
 same time; when a newer one needs room, the oldest one that is still incomplete is given up.
 */
class MediaAssembler
{
public:
	// Returns the whole message once its last missing fragment arrived; unfragmented packets are returned immediately.
	std::optional<AssembledMessage> accept(const PacketHeader &header, std::span<const uint8_t> body);

	// Messages that are being put together right now
	size_t							inProgress() const;

	// Messages that were given up because a newer one needed their room
	uint64_t						abandoned() const { return mAbandoned; }

private:
	struct Slot
	{
		bool				 used{false};
		uint64_t			 id{0};
		uint16_t			 count{0};
		uint16_t			 received{0};
		uint64_t			 present{0};	 // one bit per fragment that arrived
		uint32_t			 tag{0};
		uint32_t			 totalLength{0}; // 0 until the first fragment arrived
		uint32_t			 lastLength{0};	 // 0 until the last fragment arrived
		std::vector<uint8_t> body;
	};

	Slot								*slotFor(uint64_t id, uint16_t count);

	std::array<Slot, MediaAssemblySlots> mSlots;
	uint64_t							 mAbandoned{0};
};

} // namespace netlink::channel
