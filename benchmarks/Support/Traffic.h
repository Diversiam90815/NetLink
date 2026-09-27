/*
  ==============================================================================
	Module:         Traffic
	Description:    Application-like sending for load benchmarks
  ==============================================================================
*/

#pragma once

#include <atomic>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "BenchUtil.h"
#include "Channel/PeerChannel.h"


namespace bench
{

// Sends like an application that respects backpressure: while the send queue is full (DropNewest refuses), it retries.
// Returns how long the sender was held back, or nullopt if the message could not be queued within the timeout.
inline std::optional<Clock::duration> sendWithBackpressure(netlink::PeerChannel &channel, const std::string &to, const uint32_t type, std::span<const uint8_t> payload,
														   const netlink::DeliveryMode mode = netlink::DeliveryMode::ReliableOrdered,
														   const std::chrono::milliseconds timeout = LoadTimeout)
{
	if (channel.sendMessage(to, type, payload, mode))
		return Clock::duration::zero();

	const auto start = Clock::now();

	do
	{
		std::this_thread::yield();

		if (channel.sendMessage(to, type, payload, mode))
			return Clock::now() - start;

	} while (Clock::now() - start < timeout);

	return std::nullopt;
}


// Streams reliable messages to one peer in the background for as long as it lives, like an application saturating the channel
class BackgroundStream
{
public:
	BackgroundStream(netlink::PeerChannel &channel, std::string to, const size_t messageSize)
		: mChannel(channel), mTo(std::move(to)), mPayload(makePayload(messageSize)), mThread([this] { run(); })
	{
	}

	~BackgroundStream()
	{
		mStop.store(true);
		mThread.join();
	}

	BackgroundStream(const BackgroundStream &)			  = delete;
	BackgroundStream &operator=(const BackgroundStream &) = delete;

	uint64_t		  sent() const { return mSent.load(); }

private:
	static constexpr uint32_t StreamType = 1;

	void					  run()
	{
		while (!mStop.load())
		{
			if (sendWithBackpressure(mChannel, mTo, StreamType, mPayload, netlink::DeliveryMode::ReliableOrdered, std::chrono::milliseconds{100}))
				++mSent;
		}
	}

	netlink::PeerChannel &mChannel;
	std::string			  mTo;
	std::vector<uint8_t>  mPayload;
	std::atomic<bool>	  mStop{false};
	std::atomic<uint64_t> mSent{0};
	std::thread			  mThread; // declared last: starts once everything it uses exists
};

} // namespace bench
