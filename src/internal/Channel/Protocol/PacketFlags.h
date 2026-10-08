/*
  ==============================================================================
	Module:         PacketFlags
	Description:    Bit-packed per-packet flags of the channel wire format
  ==============================================================================
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

/*
	Packet encoding:

	bit  7     6    5    4    3..2   1..0
		[enc] [pa] [lf] [fr] [lane] [kind]

	kind : PacketKind (Data, Ack, Ping, Beacon)
	lane : Lane (Control, Reliable, Bulk, Media)
	fr   : fragmented, a fragment extension follows the header
	lf   : last fragment of a message
	pa   : on an Ack: the receiver asks the sender to pause this lane
	enc  : reserved for encrypted datagrams, must be zero
*/


namespace netlink::channel
{

enum class PacketKind : uint8_t
{
	Data   = 0, // Carries a message (or one fragment of it)
	Ack	   = 1, // Receiver confirms the Data packets of one lane
	Ping   = 2, // Asks for the Ack of a lane: keepalive, and probe of a paused lane
	Beacon = 3, // Discovery announcement: not part of a link
};

// Every lane is a stream of its own. Control, Reliable and Bulk are acknowledged and ordered, Media is neither.
enum class Lane : uint8_t
{
	Control	 = 0,
	Reliable = 1,
	Bulk	 = 2,
	Media	 = 3,
};

inline constexpr size_t LaneCount = 4;

enum class FlagBit : uint8_t
{
	Fragmented	 = 1u << 4,
	LastFragment = 1u << 5,
	Paused		 = 1u << 6,
	Encrypted	 = 1u << 7,
};


class PacketFlags
{
public:
	static constexpr uint8_t KindMask  = 0x03;
	static constexpr uint8_t LaneMask  = 0x0C;
	static constexpr uint8_t LaneShift = 2;

	constexpr PacketFlags()			   = default;

	static constexpr PacketFlags fromRaw(const uint8_t raw)
	{
		PacketFlags flags;
		flags.mBits = raw;
		return flags;
	}

	constexpr uint8_t	   raw() const { return mBits; }

	constexpr PacketKind   kind() const { return static_cast<PacketKind>(mBits & KindMask); } // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange)
	constexpr Lane		   lane() const { return static_cast<Lane>((mBits & LaneMask) >> LaneShift); }
	constexpr bool		   has(const FlagBit bit) const { return (mBits & std::to_underlying(bit)) != 0; }

	constexpr bool		   isFragmented() const { return has(FlagBit::Fragmented); }
	constexpr bool		   isLastFragment() const { return has(FlagBit::LastFragment); }
	constexpr bool		   isPaused() const { return has(FlagBit::Paused); }

	constexpr PacketFlags &setKind(const PacketKind kind)
	{
		mBits = static_cast<uint8_t>((mBits & ~KindMask) | std::to_underlying(kind));
		return *this;
	}

	constexpr PacketFlags &setLane(const Lane lane)
	{
		mBits = static_cast<uint8_t>((mBits & ~LaneMask) | (std::to_underlying(lane) << LaneShift));
		return *this;
	}

	constexpr PacketFlags &set(const FlagBit bit, const bool on = true)
	{
		if (on)
			mBits = static_cast<uint8_t>(mBits | std::to_underlying(bit));
		else
			mBits = static_cast<uint8_t>(mBits & ~std::to_underlying(bit));
		return *this;
	}

	constexpr PacketFlags &setFragment(const bool fragmented, const bool last)
	{
		set(FlagBit::Fragmented, fragmented);
		return set(FlagBit::LastFragment, fragmented && last);
	}

	// Rejects reserved bits and combinations no sender produces
	constexpr bool isValid() const
	{
		if (has(FlagBit::Encrypted))
			return false;

		if (isLastFragment() && !isFragmented())
			return false;

		if (isFragmented() && kind() != PacketKind::Data)
			return false;

		if (isPaused() && kind() != PacketKind::Ack)
			return false;

		// Media is never acknowledged, so there is nothing to confirm or to ask for
		if ((kind() == PacketKind::Ack || kind() == PacketKind::Ping) && lane() == Lane::Media)
			return false;

		if (kind() == PacketKind::Beacon && (mBits & ~KindMask) != 0)
			return false;

		return true;
	}

	constexpr bool				 operator==(const PacketFlags &other) const = default;

	// Convenience factories for the packet types a link sends
	static constexpr PacketFlags data(const Lane lane) { return PacketFlags{}.setKind(PacketKind::Data).setLane(lane); }
	static constexpr PacketFlags ack(const Lane lane, const bool paused = false) { return PacketFlags{}.setKind(PacketKind::Ack).setLane(lane).set(FlagBit::Paused, paused); }
	static constexpr PacketFlags ping(const Lane lane) { return PacketFlags{}.setKind(PacketKind::Ping).setLane(lane); }

private:
	uint8_t mBits{0};
};

} // namespace netlink::channel
