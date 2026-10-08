/*
  ==============================================================================
	Module:         Mailbox
	Description:    The only state application threads and the I/O thread of an
					engine share: messages waiting to be sent, commands and
					what the engine publishes about its peers
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

#include "NetLink/NetLink.h"

#include "Protocol/PacketFlags.h"
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
		Closed,	 // no session with the peer, or it ended while the push waited
		Stopped, // the engine does not run
	};

	enum class Command : uint8_t
	{
		Connect,
		Accept,
		Decline,
		Disconnect,
		Announce,
		StopAnnouncing,
		CheckInterface,
		ResumeReceiving,
		Shutdown,
	};

	// What the engine tells about a peer it has a session with
	enum class Session : uint8_t
	{
		None,
		Requested, // the peer asked, the application has not answered yet
		Pending,   // being opened or ended
		Connected,
	};

	struct PostedCommand
	{
		Command command;
		PeerId	peer;
	};

	struct Mail
	{
		uint32_t									tag{0};
		std::shared_ptr<const std::vector<uint8_t>> body;
	};

	struct Work
	{
		std::vector<PostedCommand> commands;
		std::vector<PeerId>		   ready; // peers that got messages since the last drain
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

	bool isRunning() const
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return mRunning;
	}

	void post(const Command command, const PeerId peer = {})
	{
		bool ring = false;
		{
			std::lock_guard<std::mutex> lock(mMutex);
			mCommands.push_back({command, peer});
			ring = !std::exchange(mSignalled, true);
		}
		ringIf(ring);
	}

	// With a timeout, a message for a full lane waits that long for room. Media never waits: its oldest message makes room.
	Push push(const PeerId peer, const Lane lane, Mail mail, const std::chrono::milliseconds timeout = {})
	{
		const size_t size = mail.body->size();

		if (size > (lane == Lane::Media ? internal::MaxMediaPayload : internal::MaxMessagePayload))
			return Push::TooLarge;

		bool ring = false;
		{
			std::unique_lock<std::mutex> lock(mMutex);

			if (!mRunning)
				return Push::Stopped;

			auto it = mPeers.find(peer);
			if (it == mPeers.end() || it->second.sealed)
				return Push::Closed;

			// A message that is larger than the whole lane is still taken when nothing else waits
			const auto hasRoom = [&] { return it->second.lane(lane).queue.empty() || it->second.lane(lane).bytes + size <= mQueueBytes; };

			if (lane != Lane::Media && !hasRoom())
			{
				if (timeout <= std::chrono::milliseconds::zero())
					return Push::Full;

				const uint64_t epoch = it->second.epoch;
				const auto	   gone	 = [&] { return it == mPeers.end() || it->second.epoch != epoch || it->second.sealed; };

				mChanged.wait_for(lock, timeout,
								  [&]
								  {
									  it = mPeers.find(peer);
									  return !mRunning || gone() || hasRoom();
								  });

				if (gone())
					return Push::Closed;

				if (!mRunning)
					return Push::Stopped;

				if (!hasRoom())
					return Push::Full;
			}

			ring = enqueue(peer, it->second, lane, std::move(mail));
		}

		ringIf(ring);
		return Push::Queued;
	}

	// One message for every connected peer, without waiting. Returns how many of them took it.
	size_t pushToAll(const Lane lane, const Mail &mail)
	{
		const size_t size = mail.body->size();

		if (size > (lane == Lane::Media ? internal::MaxMediaPayload : internal::MaxMessagePayload))
			return 0;

		size_t taken = 0;
		bool   ring	 = false;
		{
			std::lock_guard<std::mutex> lock(mMutex);

			if (!mRunning)
				return 0;

			for (auto &[id, peer] : mPeers)
			{
				const auto &[queue, bytes] = peer.lane(lane);

				if (peer.sealed || (lane != Lane::Media && !queue.empty() && bytes + size > mQueueBytes))
					continue;

				ring |= enqueue(id, peer, lane, mail);
				++taken;
			}
		}

		ringIf(ring);
		return taken;
	}

	// Waits until every acknowledged message accepted so far was acknowledged or dropped. True as well when there is no session (anymore).
	bool flush(const PeerId peer, const std::chrono::milliseconds timeout)
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

	// The session takes no further messages; what waits on Reliable and Bulk is still sent
	void seal(const PeerId peer)
	{
		{
			std::lock_guard<std::mutex> lock(mMutex);

			const auto					it = mPeers.find(peer);
			if (it == mPeers.end())
				return;

			it->second.sealed			 = true;
			it->second.lane(Lane::Media) = {};
			mSessions[peer]				 = Session::Pending;
		}

		mChanged.notify_all();
	}

	std::vector<PeerInfo> discovered() const
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return mDiscovered;
	}

	bool isDiscovered(const PeerId peer) const
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return std::ranges::any_of(mDiscovered, [&](const PeerInfo &info) { return info.id == peer; });
	}

	Session session(const PeerId peer) const
	{
		std::lock_guard<std::mutex> lock(mMutex);
		const auto					it = mSessions.find(peer);
		return it != mSessions.end() ? it->second : Session::None;
	}

	std::vector<PeerId> connected() const
	{
		std::lock_guard<std::mutex> lock(mMutex);
		std::vector<PeerId>			result;

		for (const auto &[id, state] : mSessions)
		{
			if (state == Session::Connected)
				result.push_back(id);
		}

		return result;
	}

	std::optional<PeerStats> stats(const PeerId peer) const
	{
		std::lock_guard<std::mutex> lock(mMutex);

		const auto					it = mPeers.find(peer);
		if (it == mPeers.end())
			return std::nullopt;

		PeerStats stats	   = it->second.stats;
		stats.mediaDropped = it->second.mediaDropped;

		for (const auto &lane : it->second.lanes)
			stats.bytesQueued += lane.bytes;

		return stats;
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

	// The oldest message that waits in the lane
	std::optional<Mail> take(const PeerId peer, const Lane lane)
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

			mail = std::move(queue.front());
			queue.pop_front();
			bytes -= mail->body->size();
		}

		mChanged.notify_all();
		return mail;
	}

	// The link of the peer has nothing acknowledged left to send or to wait for. False if such messages arrived in
	// the meantime: flush() keeps waiting.
	bool settle(const PeerId peer)
	{
		{
			std::lock_guard<std::mutex> lock(mMutex);

			const auto					it = mPeers.find(peer);
			if (it == mPeers.end())
				return true;

			for (const Lane lane : {Lane::Reliable, Lane::Bulk})
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

	// Connected: messages for the peer are taken from now on
	void open(const PeerId peer)
	{
		std::lock_guard<std::mutex> lock(mMutex);

		mPeers[peer]	   = Peer{};
		mPeers[peer].epoch = ++mEpochs;
		mSessions[peer]	   = Session::Connected;
	}

	// The session is over: what still waits for the peer is dropped
	void close(const PeerId peer)
	{
		{
			std::lock_guard<std::mutex> lock(mMutex);
			mPeers.erase(peer);
			mSessions.erase(peer);
			std::erase(mReady, peer);
		}

		mChanged.notify_all();
	}

	void publishSession(const PeerId peer, const Session session)
	{
		std::lock_guard<std::mutex> lock(mMutex);
		mSessions[peer] = session;
	}

	void publishDiscovered(std::vector<PeerInfo> peers)
	{
		std::lock_guard<std::mutex> lock(mMutex);
		mDiscovered = std::move(peers);
	}

	void publishStats(const PeerId peer, const PeerStats &stats)
	{
		std::lock_guard<std::mutex> lock(mMutex);

		if (const auto it = mPeers.find(peer); it != mPeers.end())
			it->second.stats = stats;
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
		uint64_t						 accepted{0};	  // acknowledged-lane messages taken so far
		uint64_t						 done{0};		  // ... of these: acknowledged or dropped
		uint64_t						 epoch{0};		  // of this session: whoever waits for another one gives up
		uint64_t						 mediaDropped{0}; // Media messages that made room for newer ones
		bool							 listed{false};	  // in mReady
		bool							 sealed{false};
		PeerStats						 stats;

		LaneQueue						&lane(const Lane lane) { return lanes[std::to_underlying(lane)]; }
	};

	// Caller holds the mutex. Returns whether the doorbell has to be rung.
	bool enqueue(const PeerId id, Peer &peer, const Lane lane, Mail mail)
	{
		auto &[queue, bytes] = peer.lane(lane);
		const size_t size	 = mail.body->size();

		if (lane == Lane::Media && queue.size() >= MediaQueueMessages)
		{
			bytes -= queue.front().body->size();
			queue.pop_front();
			++peer.mediaDropped;
		}

		queue.push_back(std::move(mail));
		bytes += size;

		if (lane != Lane::Media)
			++peer.accepted;

		if (!std::exchange(peer.listed, true))
			mReady.push_back(id);

		return !std::exchange(mSignalled, true);
	}

	void ringIf(const bool ring) const
	{
		if (ring && mDoorbell)
			mDoorbell();
	}

	std::function<void()>	   mDoorbell;

	mutable std::mutex		   mMutex;
	std::condition_variable	   mChanged;
	size_t					   mQueueBytes{DefaultSendQueueBytes};
	std::map<PeerId, Peer>	   mPeers; // the sessions that take messages
	std::vector<PostedCommand> mCommands;
	std::vector<PeerId>		   mReady;
	uint64_t				   mEpochs{0};
	bool					   mSignalled{false}; // the doorbell was rung and the work not drained yet
	bool					   mRunning{true};

	// Published by the I/O thread
	std::vector<PeerInfo>	   mDiscovered;
	std::map<PeerId, Session>  mSessions;
};

} // namespace netlink::channel
