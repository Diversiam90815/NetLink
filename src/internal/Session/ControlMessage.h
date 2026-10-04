/*
  ==============================================================================
	Module:         ControlMessage
	Description:    The messages two engines open and end a session with. They
					travel on the Control lane; the tag of the message says which
					one it is.
  ==============================================================================
*/

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Channel/Protocol/WireBuffer.h"


namespace netlink::session
{

enum class ControlType : uint32_t
{
	Hello	= 1, // always the first message of a link: Control seq 1
	Accept	= 2,
	Decline = 3,
	Close	= 4,
};

enum class DeclineReason : uint8_t
{
	Declined	 = 0,
	Incompatible = 1,
};

/*
 ext: u16 length, then fields {u8 type, u8 length, value}. A reader skips the types it does not know.
 Reserved: 1 = X25519 public key, 2 = cipher suite. This version sends none.
 */
inline void writeExt(channel::WireWriter &out)
{
	out.u16(0);
}

inline bool skipExt(channel::WireReader &in)
{
	channel::WireReader fields(in.next(in.u16()));

	while (!in.failed() && !fields.complete())
	{
		fields.u8();
		fields.next(fields.u8());

		if (fields.failed())
			return false;
	}

	return !in.failed();
}


struct Hello
{
	uint64_t	fromInstance{0};
	uint64_t	toInstance{0};
	uint64_t	appIdHash{0};
	uint16_t	verMajor{0};
	uint16_t	verMinor{0};
	std::string name;
};

struct Accept
{
	std::string name;
};

struct Decline
{
	DeclineReason reason{DeclineReason::Declined};
	std::string	  text;
};


inline std::vector<uint8_t> encode(const Hello &hello)
{
	channel::WireWriter out;
	out.u64(hello.fromInstance);
	out.u64(hello.toInstance);
	out.u64(hello.appIdHash);
	out.u16(hello.verMajor);
	out.u16(hello.verMinor);
	out.text(hello.name);
	writeExt(out);
	return out.take();
}

inline std::optional<Hello> decodeHello(const std::span<const uint8_t> body)
{
	channel::WireReader in(body);
	Hello				hello;
	hello.fromInstance = in.u64();
	hello.toInstance   = in.u64();
	hello.appIdHash	   = in.u64();
	hello.verMajor	   = in.u16();
	hello.verMinor	   = in.u16();
	hello.name		   = in.text();

	if (!skipExt(in) || !in.complete() || hello.fromInstance == 0)
		return std::nullopt;

	return hello;
}


inline std::vector<uint8_t> encode(const Accept &accept)
{
	channel::WireWriter out;
	out.text(accept.name);
	writeExt(out);
	return out.take();
}

inline std::optional<Accept> decodeAccept(const std::span<const uint8_t> body)
{
	channel::WireReader in(body);
	Accept				accept{.name = in.text()};

	if (!skipExt(in) || !in.complete())
		return std::nullopt;

	return accept;
}


inline std::vector<uint8_t> encode(const Decline &decline)
{
	channel::WireWriter out;
	out.u8(static_cast<uint8_t>(decline.reason));
	out.text(decline.text);
	return out.take();
}

inline std::optional<Decline> decodeDecline(const std::span<const uint8_t> body)
{
	channel::WireReader in(body);
	const uint8_t		reason = in.u8();
	Decline				decline{.reason = reason == static_cast<uint8_t>(DeclineReason::Incompatible) ? DeclineReason::Incompatible : DeclineReason::Declined, .text = in.text()};

	if (!in.complete())
		return std::nullopt;

	return decline;
}


// Close carries one byte, the reason. A receiver has no use for it yet: the remote ended the session.
inline std::vector<uint8_t> encodeClose(const uint8_t reason = 0)
{
	return {reason};
}

} // namespace netlink::session
