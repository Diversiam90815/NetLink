/*
  ==============================================================================
	Module:         PacketFlags
	Description:    Bit-packed per-packet flags of the channel wire format
  ==============================================================================
*/

#pragma once

#include <utility>

/*
	Packet encoding:

	bit  7    6    5    4    3    2..0
		[res][ch] [lf] [fr] [rl] [kind]

	kind : PacketKind (Data, DataAck, AckAck, Heartbeat)
	rl   : reliable (acknowledged) / unreliable
	fr   : fragmented, a fragment extension follows the header
	lf   : last fragment of a message
	ch   : Control / Application channel
	res  : reserved, must be zero
*/


namespace netlink::channel
{

enum class PacketKind : uint8_t
{
	Data	  = 0, // Carries a message (or one fragment of it)
	DataAck	  = 1, // Receiver confirms a reliable Data packet
	AckAck	  = 2, // Sender confirms it got the DataAck, the receiver can forget the key
	Heartbeat = 3, // Keepalive while a session is idle, never acknowledged
};

enum class ChannelId : uint8_t
{
	Control,	 // Validation and connection flow signals
	Application, // Messages of the application using NetLink
};

enum class FlagBit : uint8_t
{
	Reliable	 = 1u << 3,
	Fragmented	 = 1u << 4,
	LastFragment = 1u << 5,
	Application	 = 1u << 6,
	Reserved	 = 1u << 7,
};


class PacketFlags
{
public:
	static constexpr uint8_t KindMask	 = 0x07;
	static constexpr uint8_t MaxKindBits = std::to_underlying(PacketKind::Heartbeat);

	constexpr PacketFlags()				 = default;

	static constexpr PacketFlags fromRaw(const uint8_t raw)
	{
		PacketFlags flags;
		flags.mBits = raw;
		return flags;
	}

	constexpr uint8_t	   raw() const { return mBits; }

	constexpr PacketKind   kind() const { return static_cast<PacketKind>(mBits & KindMask); }
	constexpr bool		   has(const FlagBit bit) const { return (mBits & std::to_underlying(bit)) != 0; }

	constexpr bool		   isReliable() const { return has(FlagBit::Reliable); }
	constexpr bool		   isFragmented() const { return has(FlagBit::Fragmented); }
	constexpr bool		   isLastFragment() const { return has(FlagBit::LastFragment); }
	constexpr ChannelId	   channel() const { return has(FlagBit::Application) ? ChannelId::Application : ChannelId::Control; }

	constexpr PacketFlags &setKind(const PacketKind kind)
	{
		mBits = static_cast<uint8_t>((mBits & ~KindMask) | (std::to_underlying(kind) & KindMask));
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

	constexpr PacketFlags &setReliable(const bool reliable = true) { return set(FlagBit::Reliable, reliable); }
	constexpr PacketFlags &setChannel(const ChannelId channel) { return set(FlagBit::Application, channel == ChannelId::Application); }

	constexpr PacketFlags &setFragment(const bool fragmented, const bool last)
	{
		set(FlagBit::Fragmented, fragmented);
		return set(FlagBit::LastFragment, fragmented && last);
	}

	// Rejects reserved kinds/bits and combinations no sender produces
	constexpr bool isValid() const
	{
		if ((mBits & KindMask) > MaxKindBits || has(FlagBit::Reserved))
			return false;

		if (isLastFragment() && !isFragmented())
			return false;

		// Only reliable Data packets are fragmented: losing one fragment of an unreliable message would lose all of it
		if (isFragmented() && (kind() != PacketKind::Data || !isReliable()))
			return false;

		// Acknowledgements only exist for reliable packets, heartbeats never are
		if ((kind() == PacketKind::DataAck || kind() == PacketKind::AckAck) && !isReliable())
			return false;

		if (kind() == PacketKind::Heartbeat && isReliable())
			return false;

		return true;
	}

	constexpr bool				 operator==(const PacketFlags &other) const = default;

	// Convenience factories for the packet types a link sends
	static constexpr PacketFlags data(const ChannelId channel, const bool reliable) { return PacketFlags{}.setKind(PacketKind::Data).setReliable(reliable).setChannel(channel); }
	static constexpr PacketFlags ack(const PacketKind kind) { return PacketFlags{}.setKind(kind).setReliable(); }
	static constexpr PacketFlags heartbeat() { return PacketFlags{}.setKind(PacketKind::Heartbeat); }

private:
	uint8_t mBits{0};
};

} // namespace netlink::channel
