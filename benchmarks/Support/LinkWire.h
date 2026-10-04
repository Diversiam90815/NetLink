/*
  ==============================================================================
	Module:         LinkWire
	Description:    Two production ReliableLinks back to back under a manual
					clock. ReliableLink does no I/O itself: the wire only moves
					the datagrams one link produced into the other one.
  ==============================================================================
*/

#pragma once

#include <algorithm>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "Channel/Reliability/ReliableLink.h"


namespace bench
{

class LinkWire
{
public:
	using ReliableLink = netlink::channel::ReliableLink;
	using PacketHeader = netlink::channel::PacketHeader;

	// Decides per packet whether it is lost on the way (true = lost)
	using Drop		   = std::function<bool(const PacketHeader &header)>;

	explicit LinkWire(const netlink::channel::LinkTimings &timings = {}) : a(timings), b(timings) {}

	// Queues a reliable message for a to send
	void send(std::span<const uint8_t> payload) { mFromA.waiting.push_back({.tag = 0, .body = std::make_shared<const std::vector<uint8_t>>(payload.begin(), payload.end())}); }

	// Loses every n-th Data packet on the way, retransmissions included (deterministic). n <= 0: lossless.
	static Drop dropEveryNth(const int64_t n)
	{
		if (n <= 0)
			return {};

		return [n, count = int64_t{0}](const PacketHeader &header) mutable { return header.flags.kind() == netlink::channel::PacketKind::Data && ++count % n == 0; };
	}

	// Carries everything `from` produced so far into `to`. Returns the number of datagrams moved (lost ones included).
	size_t transfer(ReliableLink &from, ReliableLink &to, const Drop &drop = {})
	{
		const auto datagrams = from.takeOutgoing(now, &from == &a ? mFromA : mFromB);

		for (const auto &datagram : datagrams)
		{
			// Like the socket, the wire takes header and body as they are: nothing is joined into one buffer first
			auto packet = netlink::channel::decodeHeader(datagram.header());
			if (!packet || (drop && drop(packet->header)))
				continue;

			packet->body = datagram.body();
			to.onPacket(*packet, now);
		}

		return datagrams.size();
	}

	// Exchanges packets until both links are idle, advancing the clock to the next retransmission deadline whenever the wire runs dry.
	// Returns false if the links did not settle.
	bool settle(const Drop &dropAtoB = {}, const Drop &dropBtoA = {})
	{
		for (int round = 0; round < MaxRounds; ++round)
		{
			if (transfer(a, b, dropAtoB) + transfer(b, a, dropBtoA) > 0)
				continue;

			if (!a.hasPendingReliable() && !b.hasPendingReliable() && mFromA.waiting.empty())
				return true;

			// Nothing on the wire but data still unacknowledged: jump to the next retransmission
			const auto deadline = earliestDeadline();
			if (!deadline)
				return false;

			now = *deadline;
			a.onTimer(now);
			b.onTimer(now);
		}

		return false;
	}

	// Payload bytes delivered at b since the last call
	size_t takeDeliveredBytesAtB()
	{
		size_t bytes = 0;
		for (const auto &message : b.takeDelivered())
			bytes += message.body.size();
		return bytes;
	}

	ReliableLink			a;
	ReliableLink			b;
	ReliableLink::TimePoint now = ReliableLink::Clock::now();

private:
	static constexpr int MaxRounds = 1'000'000;

	// What a link in production takes from the mailbox: here the messages of the Reliable lane, in memory
	struct Outbox final : netlink::channel::MessageSource
	{
		std::optional<netlink::channel::OutboundMessage> next(const netlink::channel::Lane lane) override
		{
			if (lane != netlink::channel::Lane::Reliable || waiting.empty())
				return std::nullopt;

			auto message = std::move(waiting.front());
			waiting.pop_front();
			return message;
		}

		std::deque<netlink::channel::OutboundMessage> waiting;
	};

	Outbox								   mFromA;
	Outbox								   mFromB;

	std::optional<ReliableLink::TimePoint> earliestDeadline() const
	{
		const auto first  = a.nextDeadline();
		const auto second = b.nextDeadline();

		if (first && second)
			return std::min(*first, *second);

		return first ? first : second;
	}
};

} // namespace bench
