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
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

#include "Protocol/PacketFlags.h"
#include "Socket/SocketTypes.h"
#include "TransportConstants.h"


namespace netlink::channel
{

/*
 Application threads only push: messages into the lanes of a peer, commands for everything else. The I/O thread
 drains the commands, takes the messages one by one as its links are able to send them and reports back what was
 acknowledged.

 The doorbell is rung when work arrives and none was waiting. It must wake the I/O thread, also when that thread is
 not waiting yet.
 */
class Mailbox
{
public:
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
		ResumeReceiving,
	};

	struct PostedCommand
	{
		Command			   command;
		net::SocketAddress peer;
	};

	struct Mail
	{
		uint32_t									tag{0};
		std::shared_ptr<const std::vector<uint8_t>> body;
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

	// Bytes of messages that may wait in one acknowledged lane of one peer
	void	 setQueueBytes(const size_t bytes)
	{
		std::lock_guard<std::mutex> lock(mMutex);
		mQueueBytes = bytes;
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

	// Everything but control signals that was not taken yet
	void dropApplication(const net::SocketAddress &peer)
	{
		bool ring = false;
		{
			std::lock_guard<std::mutex> lock(mMutex);

			const auto					it = mPeers.find(peer);
			if (it == mPeers.end())
				return;

			for (const Lane lane : {Lane::Reliable, Lane::Bulk, Lane::Media})
				it->second.lane(lane) = {};

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

	// With a timeout, a message for a full lane waits that long for room. Media never waits: its oldest message makes room.
	Push push(const net::SocketAddress &peer, const Lane lane, Mail mail, const std::chrono::milliseconds timeout = {})
	{
		const size_t size = mail.body->size();

		if (size > (lane == Lane::Media ? internal::MaxMediaPayload : internal::MaxMessagePayload))
			return Push::TooLarge;

		bool ring = false;
		{
			std::unique_lock<std::mutex> lock(mMutex);

			auto						 it = mPeers.find(peer);
			if (it == mPeers.end())
				return Push::Closed;

			// A message that is larger than the whole lane is still taken when nothing else waits
			const auto hasRoom = [&] { return it->second.lane(lane).queue.empty() || it->second.lane(lane).bytes + size <= mQueueBytes; };

			if (lane != Lane::Media && !hasRoom())
			{
				if (timeout <= std::chrono::milliseconds::zero() || !mRunning)
					return Push::Full;

				const uint64_t epoch = it->second.epoch;

				mChanged.wait_for(lock, timeout,
								  [&]
								  {
									  it = mPeers.find(peer);
									  return !mRunning || it == mPeers.end() || it->second.epoch != epoch || hasRoom();
								  });

				if (it == mPeers.end() || it->second.epoch != epoch)
					return Push::Closed;

				if (!hasRoom())
					return Push::Full;
			}

			auto &[queue, bytes] = it->second.lane(lane);

			if (lane == Lane::Media && queue.size() >= MediaQueueMessages)
			{
				bytes -= queue.front().body->size();
				queue.pop_front();
			}

			queue.push_back(std::move(mail));
			bytes += size;

			if (lane != Lane::Media)
				++it->second.accepted;

			if (!std::exchange(it->second.listed, true))
				mReady.push_back(peer);

			ring = !std::exchange(mSignalled, true);
		}

		ringIf(ring);
		return Push::Queued;
	}

	// Waits until every acknowledged message accepted so far was acknowledged or dropped. True as well when the peer is not open (anymore).
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

	// The oldest message that waits in the lane. Nothing is handed over while commands are waiting: they come first,
	// and the peer is offered again with them.
	std::optional<Mail> take(const net::SocketAddress &peer, const Lane lane)
	{
		std::optional<Mail> mail;
		{
			std::lock_guard<std::mutex> lock(mMutex);

			const auto					it = mPeers.find(peer);
			if (it == mPeers.end())
				return std::nullopt;

			auto &[queue, bytes] = it->second.lane(lane);
			if (queue.empty())
				return std::nullopt;

			if (!mCommands.empty())
			{
				if (!std::exchange(it->second.listed, true))
					mReady.push_back(peer);

				return std::nullopt;
			}

			mail = std::move(queue.front());
			queue.pop_front();
			bytes -= mail->body->size();
		}

		mChanged.notify_all();
		return mail;
	}

	// The link of the peer has nothing acknowledged left to send or to wait for. False if such messages arrived in
	// the meantime: flush() keeps waiting.
	bool settle(const net::SocketAddress &peer)
	{
		{
			std::lock_guard<std::mutex> lock(mMutex);

			const auto					it = mPeers.find(peer);
			if (it == mPeers.end())
				return true;

			for (const Lane lane : {Lane::Control, Lane::Reliable, Lane::Bulk})
			{
				if (!it->second.lane(lane).queue.empty())
					return false;
			}

			if (it->second.done == it->second.accepted)
				return true;

			it->second.done = it->second.accepted;
		}

		mChanged.notify_all();
		return true;
	}

private:
	struct LaneQueue
	{
		std::deque<Mail> queue;
		size_t			 bytes{0};
	};

	struct Peer
	{
		std::array<LaneQueue, LaneCount> lanes;
		uint64_t						 accepted{0};	// acknowledged-lane messages taken so far
		uint64_t						 done{0};		// ... of these: acknowledged or dropped
		uint64_t						 epoch{0};		// changes when the peer is reset: whoever waits gives up
		bool							 listed{false}; // in mReady

		LaneQueue						&lane(const Lane lane) { return lanes[std::to_underlying(lane)]; }
	};

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
	size_t							   mQueueBytes{DefaultSendQueueBytes};
	std::map<net::SocketAddress, Peer> mPeers;
	std::vector<PostedCommand>		   mCommands;
	std::vector<net::SocketAddress>	   mReady;
	uint64_t						   mEpochs{0};
	bool							   mSignalled{false}; // the doorbell was rung and the work not drained yet
	bool							   mRunning{false};
};

} // namespace netlink::channel
