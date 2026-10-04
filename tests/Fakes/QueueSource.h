/*
  ==============================================================================
	Module:         QueueSource
	Description:    Messages waiting to be sent, per lane: what the mailbox is
					to a link in production
  ==============================================================================
*/

#pragma once

#include <array>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

#include "Channel/Reliability/ReliableLink.h"


namespace FakeNet
{

class QueueSource final : public netlink::channel::MessageSource
{
public:
	using Lane = netlink::channel::Lane;

	void push(const Lane lane, const uint32_t tag, std::vector<uint8_t> body)
	{
		queue(lane).push_back({.tag = tag, .body = std::make_shared<const std::vector<uint8_t>>(std::move(body))});
	}

	std::optional<netlink::channel::OutboundMessage> next(const Lane lane) override
	{
		auto &waiting = queue(lane);
		if (waiting.empty())
			return std::nullopt;

		auto message = std::move(waiting.front());
		waiting.pop_front();
		return message;
	}

	// Messages the link did not take yet
	size_t waiting(const Lane lane) const { return mQueues[std::to_underlying(lane)].size(); }

	bool   empty() const
	{
		return std::ranges::all_of(mQueues, [](const auto &waiting) { return waiting.empty(); });
	}

private:
	std::deque<netlink::channel::OutboundMessage>										  &queue(const Lane lane) { return mQueues[std::to_underlying(lane)]; }

	std::array<std::deque<netlink::channel::OutboundMessage>, netlink::channel::LaneCount> mQueues;
};

// Everything a link has to send right now, in one pass: Acks and Pings first, then the lanes in order of urgency
inline std::vector<netlink::channel::OutgoingDatagram> takeOutgoing(netlink::channel::ReliableLink &link, const netlink::channel::ReliableLink::TimePoint now,
																	netlink::channel::MessageSource &source)
{
	using netlink::channel::Lane;

	std::vector<netlink::channel::OutgoingDatagram> pass;

	while (const auto *ack = link.peekAck())
	{
		pass.push_back(*ack);
		link.commitAck();
	}

	for (const Lane lane : {Lane::Control, Lane::Media, Lane::Reliable, Lane::Bulk})
	{
		while (const auto *datagram = link.peek(lane, now, source))
		{
			pass.push_back(*datagram);
			link.commit(lane, now);
		}
	}

	return pass;
}

} // namespace FakeNet
