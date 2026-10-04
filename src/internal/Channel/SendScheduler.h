/*
  ==============================================================================
	Module:         SendScheduler
	Description:    Hands out the send budget of one socket: a token bucket
					counted in datagrams, shared by lane and, within a lane,
					by the peers in turn
  ==============================================================================
*/

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "Protocol/PacketFlags.h"
#include "Socket/SocketTypes.h"


namespace netlink::channel
{

class SendScheduler
{
public:
	using Clock						= std::chrono::steady_clock;
	using TimePoint					= Clock::time_point;
	using Peer						= net::SocketAddress;

	// Datagrams a peer sends before the next one of its lane gets its turn
	static constexpr size_t Quantum = 4;

	// How long the owner waits before it runs the scheduler again while datagrams are waiting for tokens
	static constexpr auto	Tick	= std::chrono::milliseconds{1};

	// The most urgent lane first
	static constexpr Lane	Order[] = {Lane::Control, Lane::Media, Lane::Reliable, Lane::Bulk};

	// What send() did with its turn
	enum class Result
	{
		Sent,
		Lost,	 // counts as sent, but the peer's turn ends: the destination cannot be reached right now
		Empty,	 // the peer has nothing in this lane: it leaves the ring
		Blocked, // the socket takes nothing anymore: the run ends
	};

	// ratePerSecond in datagrams, 0 = unlimited
	explicit SendScheduler(const uint32_t ratePerSecond = 0) { setRate(ratePerSecond); }

	void setRate(const uint32_t ratePerSecond)
	{
		mRate	= ratePerSecond;
		mBurst	= burstOf(ratePerSecond);
		mTokens = static_cast<double>(mBurst);
		mRefilledAt.reset();
	}

	// Two ticks worth of datagrams: a wake-up that comes late does not cost budget
	static constexpr size_t burstOf(const uint32_t ratePerSecond) { return std::max<size_t>(16, ratePerSecond / 500); }

	size_t					burst() const { return mBurst; }
	bool					isUnlimited() const { return mRate == 0; }
	double					tokens() const { return mTokens; }
	bool					hasTokens() const { return isUnlimited() || mTokens >= 1.0; }

	void					refill(const TimePoint now)
	{
		if (mRefilledAt && now > *mRefilledAt)
			mTokens = std::min(static_cast<double>(mBurst), mTokens + mRate * std::chrono::duration<double>(now - *mRefilledAt).count());

		mRefilledAt = now;
	}

	// Also for what is sent without asking, like acknowledgements: the budget may go below zero
	void spend(const size_t datagrams = 1)
	{
		if (!isUnlimited())
			mTokens -= static_cast<double>(datagrams);
	}

	// The peer has something to send in that lane. Must not be added twice: it leaves with Result::Empty or remove().
	void add(const Lane lane, const Peer &peer) { ring(lane).peers.push_back(peer); }

	void remove(const Peer &peer)
	{
		for (auto &ring : mRings)
		{
			const auto it = std::ranges::find(ring.peers, peer);
			if (it == ring.peers.end())
				continue;

			if (static_cast<size_t>(it - ring.peers.begin()) < ring.cursor)
				--ring.cursor;

			ring.peers.erase(it);
		}
	}

	void clear()
	{
		for (auto &ring : mRings)
			ring = {};
	}

	// Datagrams are waiting for tokens or for the socket: run again a tick later
	bool hasBacklog() const
	{
		return std::ranges::any_of(mRings, [](const Ring &ring) { return !ring.peers.empty(); });
	}

	// Asks send(lane, peer) for one datagram at a time: the most urgent lane first, the peers of a lane in turn, for as
	// long as tokens are left.
	template <typename Send>
	void run(Send &&send)
	{
		// Media must not use up the budget retransmissions need
		const bool acknowledgedWaits = !ring(Lane::Reliable).peers.empty() || !ring(Lane::Bulk).peers.empty();
		double	   mediaAllowance	 = acknowledgedWaits ? mTokens * 3.0 / 4.0 : mTokens;

		for (const Lane lane : Order)
		{
			Ring	  &ring	  = this->ring(lane);
			const bool capped = lane == Lane::Media && !isUnlimited();

			while (!ring.peers.empty() && hasTokens() && (!capped || mediaAllowance >= 1.0))
			{
				if (ring.cursor >= ring.peers.size())
					ring.cursor = 0;

				switch (send(lane, Peer{ring.peers[ring.cursor]}))
				{
				case Result::Sent:
					spend();
					mediaAllowance -= capped ? 1.0 : 0.0;

					if (++ring.used >= Quantum)
						ring.next();
					break;

				case Result::Lost:
					spend();
					mediaAllowance -= capped ? 1.0 : 0.0;
					ring.next();
					break;

				case Result::Empty:
					ring.peers.erase(ring.peers.begin() + static_cast<std::ptrdiff_t>(ring.cursor));
					ring.used = 0;
					break;

				case Result::Blocked: return;
				}
			}
		}
	}

private:
	struct Ring
	{
		std::vector<Peer> peers;
		size_t			  cursor{0}; // whose turn it is: kept between runs
		size_t			  used{0};	 // ... and how much of its quantum is gone

		void			  next()
		{
			++cursor;
			used = 0;
		}
	};

	Ring					   &ring(const Lane lane) { return mRings[std::to_underlying(lane)]; }

	uint32_t					mRate{0};
	size_t						mBurst{0};
	double						mTokens{0};
	std::optional<TimePoint>	mRefilledAt;
	std::array<Ring, LaneCount> mRings;
};

} // namespace netlink::channel
