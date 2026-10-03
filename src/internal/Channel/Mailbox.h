/*
  ==============================================================================
	Module:         Mailbox
	Description:    The only state application threads and the I/O thread of a
					channel share: messages waiting to be sent, commands and
					what waiting callers need to know
  ==============================================================================
*/

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <ranges>
#include <utility>
#include <vector>

#include "NetLink/NetLink.h"
#include "Socket/SocketTypes.h"


namespace netlink::channel
{

/*
 Application threads only push: messages into the lanes of a peer, commands for everything else. The I/O thread
 drains the commands, feeds the messages into its links as they have room and reports back what was acknowledged.

 The doorbell is rung when work arrives and none was waiting. It must wake the I/O thread, also when that thread is
 not waiting yet.
 */
class Mailbox
{
public:
	enum class Lane : uint8_t
	{
		Control,
		Application,
		Unreliable,
	};

	enum class Push
	{
		Queued,
		Full,
		TooLarge,
		Closed, // the peer is unknown, or was closed or reset while the push waited
	};

	enum class Command : uint8_t
	{
		EraseLink,
		ResetLinks,
		DropApplication,
		KeepAliveOn,
		KeepAliveOff,
		Reconfigure,
	};

	struct PostedCommand
	{
		Command			   command;
		net::SocketAddress peer;
	};

	struct Mail
	{
		uint32_t			 tag{0};
		std::vector<uint8_t> body;
	};

	struct Limits
	{
		size_t		   controlCapacity{0};
		size_t		   applicationCapacity{0};
		OverflowPolicy applicationOverflow{OverflowPolicy::DropNewest};
		size_t		   unreliableCapacity{0};
		size_t		   maxMessageSize{0};
		size_t		   maxUnreliableBody{0};
	};

	struct Work
	{
		std::vector<PostedCommand>		commands;
		std::vector<net::SocketAddress> ready; // peers that got messages since the last drain
	};

	explicit Mailbox(std::function<void()> doorbell) : mDoorbell(std::move(doorbell)) {}

	Mailbox(const Mailbox &)			= delete;
	Mailbox &operator=(const Mailbox &) = delete;


	// --- Any thread ------------------------------------------------------------

	void	 setLimits(const Limits &limits)
	{
		std::lock_guard<std::mutex> lock(mMutex);
		mLimits = limits;
	}

	void setRunning(const bool running)
	{
		{
			std::lock_guard<std::mutex> lock(mMutex);
			mRunning = running;
		}
		mChanged.notify_all();
	}

	void open(const net::SocketAddress &peer)
	{
		std::lock_guard<std::mutex> lock(mMutex);

		if (!mPeers.contains(peer))
			mPeers[peer].epoch = ++mEpochs;
	}

	void close(const net::SocketAddress &peer)
	{
		bool ring = false;
		{
			std::lock_guard<std::mutex> lock(mMutex);

			if (mPeers.erase(peer) == 0)
				return;

			ring = postLocked(Command::EraseLink, peer);
		}

		mChanged.notify_all();
		ringIf(ring);
	}

	// Drops everything that is queued. The peers stay open.
	void reset()
	{
		bool ring = false;
		{
			std::lock_guard<std::mutex> lock(mMutex);

			for (auto &peer : mPeers | std::views::values)
			{
				peer	   = Peer{};
				peer.epoch = ++mEpochs;
			}

			mReady.clear();
			ring = postLocked(Command::ResetLinks, {});
		}

		mChanged.notify_all();
		ringIf(ring);
	}

	void dropApplication(const net::SocketAddress &peer)
	{
		bool ring = false;
		{
			std::lock_guard<std::mutex> lock(mMutex);

			const auto					it = mPeers.find(peer);
			if (it == mPeers.end())
				return;

			it->second.lane(Lane::Application).clear();
			it->second.lane(Lane::Unreliable).clear();
			ring = postLocked(Command::DropApplication, peer);
		}

		mChanged.notify_all();
		ringIf(ring);
	}

	void post(const Command command, const net::SocketAddress &peer = {})
	{
		bool ring = false;
		{
			std::lock_guard<std::mutex> lock(mMutex);
			ring = postLocked(command, peer);
		}
		ringIf(ring);
	}

	// With a timeout, a message for a full lane waits that long for room
	Push push(const net::SocketAddress &peer, const Lane lane, Mail mail, const std::chrono::milliseconds timeout = {})
	{
		bool ring = false;
		{
			std::unique_lock<std::mutex> lock(mMutex);

			if (mail.body.size() > (lane == Lane::Unreliable ? mLimits.maxUnreliableBody : mLimits.maxMessageSize))
				return Push::TooLarge;

			auto it = mPeers.find(peer);
			if (it == mPeers.end())
				return Push::Closed;

			const size_t capacity	 = std::max<size_t>(capacityOf(lane), 1);
			const bool	 dropsOldest = lane == Lane::Unreliable || (lane == Lane::Application && mLimits.applicationOverflow == OverflowPolicy::DropOldest);

			if (!dropsOldest && it->second.lane(lane).size() >= capacity)
			{
				if (timeout <= std::chrono::milliseconds::zero() || !mRunning)
					return Push::Full;

				const uint64_t epoch = it->second.epoch;

				mChanged.wait_for(lock, timeout,
								  [&]
								  {
									  it = mPeers.find(peer);
									  return !mRunning || it == mPeers.end() || it->second.epoch != epoch || it->second.lane(lane).size() < capacity;
								  });

				if (it == mPeers.end() || it->second.epoch != epoch)
					return Push::Closed;

				if (it->second.lane(lane).size() >= capacity)
					return Push::Full;
			}

			auto &queue = it->second.lane(lane);
			if (queue.size() >= capacity)
				queue.pop_front();

			queue.push_back(std::move(mail));

			if (lane != Lane::Unreliable)
				++it->second.accepted;

			if (!std::exchange(it->second.listed, true))
				mReady.push_back(peer);

			ring = !std::exchange(mSignalled, true);
		}

		ringIf(ring);
		return Push::Queued;
	}

	// Waits until every reliable message accepted so far was acknowledged or dropped. True as well when the peer is not open (anymore).
	bool flush(const net::SocketAddress &peer, const std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(mMutex);

		auto						 it = mPeers.find(peer);
		if (it == mPeers.end())
			return true;

		const uint64_t epoch  = it->second.epoch;
		const uint64_t target = it->second.accepted;

		mChanged.wait_for(lock, timeout,
						  [&]
						  {
							  it = mPeers.find(peer);
							  return !mRunning || it == mPeers.end() || it->second.epoch != epoch || it->second.done >= target;
						  });

		return it == mPeers.end() || it->second.epoch != epoch || it->second.done >= target;
	}


	// --- I/O thread ------------------------------------------------------------

	// Takes the commands and the peers with new messages. From here on the doorbell rings again.
	void drain(Work &work)
	{
		work.commands.clear();
		work.ready.clear();

		std::lock_guard<std::mutex> lock(mMutex);

		work.commands.swap(mCommands);
		work.ready.swap(mReady);

		for (const auto &peer : work.ready)
		{
			if (const auto it = mPeers.find(peer); it != mPeers.end())
				it->second.listed = false;
		}

		mSignalled = false;
	}

	// Hands the waiting messages of a peer to sink(lane, mail), oldest first, for as long as it returns true. Nothing
	// is handed over while commands are waiting: they come first. Returns whether messages are still waiting.
	template <typename Sink>
	bool feed(const net::SocketAddress &peer, Sink &&sink)
	{
		bool madeRoom = false;
		bool waiting  = false;
		{
			std::lock_guard<std::mutex> lock(mMutex);

			const auto					it = mPeers.find(peer);
			if (it == mPeers.end())
				return false;

			if (!mCommands.empty())
			{
				if (it->second.hasMail() && !std::exchange(it->second.listed, true))
					mReady.push_back(peer);

				return it->second.hasMail();
			}

			for (const Lane lane : {Lane::Control, Lane::Application, Lane::Unreliable})
			{
				auto &queue = it->second.lane(lane);

				while (!queue.empty() && sink(lane, queue.front()))
				{
					queue.pop_front();
					madeRoom = true;
				}
			}

			waiting = it->second.hasMail();
		}

		if (madeRoom)
			mChanged.notify_all();

		return waiting;
	}

	// The link of the peer has nothing reliable left to send or to wait for. False if reliable messages arrived in the
	// meantime: flush() keeps waiting.
	bool settle(const net::SocketAddress &peer)
	{
		{
			std::lock_guard<std::mutex> lock(mMutex);

			const auto					it = mPeers.find(peer);
			if (it == mPeers.end())
				return true;

			if (!it->second.lane(Lane::Control).empty() || !it->second.lane(Lane::Application).empty())
				return false;

			if (it->second.done == it->second.accepted)
				return true;

			it->second.done = it->second.accepted;
		}

		mChanged.notify_all();
		return true;
	}

private:
	struct Peer
	{
		std::array<std::deque<Mail>, 3> lanes;
		uint64_t						accepted{0};   // reliable messages taken so far
		uint64_t						done{0};	   // ... of these: acknowledged or dropped
		uint64_t						epoch{0};	   // changes when the peer is reset: whoever waits gives up
		bool							listed{false}; // in mReady

		std::deque<Mail>			   &lane(const Lane lane) { return lanes[std::to_underlying(lane)]; }
		bool							hasMail() const
		{
			return std::ranges::any_of(lanes, [](const auto &queue) { return !queue.empty(); });
		}
	};

	size_t capacityOf(const Lane lane) const
	{
		switch (lane)
		{
		case Lane::Control: return mLimits.controlCapacity;
		case Lane::Application: return mLimits.applicationCapacity;
		case Lane::Unreliable: return mLimits.unreliableCapacity;
		}
		return 0;
	}

	bool postLocked(const Command command, const net::SocketAddress &peer)
	{
		mCommands.push_back({command, peer});
		return !std::exchange(mSignalled, true);
	}

	void ringIf(const bool ring) const
	{
		if (ring && mDoorbell)
			mDoorbell();
	}

	std::function<void()>			   mDoorbell;

	std::mutex						   mMutex;
	std::condition_variable			   mChanged;
	Limits							   mLimits;
	std::map<net::SocketAddress, Peer> mPeers;
	std::vector<PostedCommand>		   mCommands;
	std::vector<net::SocketAddress>	   mReady;
	uint64_t						   mEpochs{0};
	bool							   mSignalled{false}; // the doorbell was rung and the work not drained yet
	bool							   mRunning{false};
};

} // namespace netlink::channel
