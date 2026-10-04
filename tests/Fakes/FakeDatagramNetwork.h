/*
  ==============================================================================
	Module:         FakeDatagramNetwork
	Description:    In-memory datagram network for deterministic tests of
					datagram based services (no real ports, no OS sockets).
  ==============================================================================
*/

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "Socket/IDatagramSocket.h"
#include "TestIp.h"
#include "Util/Timing/DeadlineTimer.h"


namespace FakeNet
{

using namespace netlink::net;

inline constexpr const char *BroadcastAddress = "255.255.255.255";

using Clock									  = std::chrono::steady_clock;
using TimePoint								  = Clock::time_point;


// One address of a host, with the subnet it is in
struct Interface
{
	IPv4Address ip;
	IPv4Address mask{IPv4Address::fromHostOrder(0xFFFFFF00u)};

	bool		reaches(const IPv4Address &other) const { return (other.toHostOrder() & mask.toHostOrder()) == (ip.toHostOrder() & mask.toHostOrder()); }
	IPv4Address broadcast() const { return IPv4Address::fromHostOrder(ip.toHostOrder() | ~mask.toHostOrder()); }
};


// What happens to datagrams on their way. By default nothing: they arrive at once and in order.
struct LinkProfile
{
	std::chrono::microseconds latency{0};
	uint64_t				  bandwidth{0};			// bytes per second a socket puts on the wire, 0 = unlimited
	size_t					  queueLimit{0};		// datagrams of a socket that may wait for the wire, 0 = unlimited
	bool					  blockWhenFull{false}; // a full queue refuses the datagram (WouldBlock) instead of dropping it
	double					  lossRate{0.0};
	double					  reorderRate{0.0};		// share of the datagrams that arrive reorderDelay late
	std::chrono::microseconds reorderDelay{500};
	uint32_t				  seed{1};
};


/*
 Hosts with one or more addresses on subnets, and the sockets bound on them:
	- a unicast datagram reaches one socket: the first one bound to that port on the host that owns the address
	- a broadcast (limited or of a subnet) reaches every socket on the port on every host of that subnet, the sender's included
	- a socket bound to no particular address sends from the address of the iface that reaches the destination

 The network runs on the real clock, or on a virtual one that only its owner moves (SimDriver): then nothing ever blocks.
 */
class FakeDatagramNetwork : public std::enable_shared_from_this<FakeDatagramNetwork>
{
public:
	using Tap = std::function<void(const SocketAddress &from, const SocketAddress &to, std::span<const uint8_t> data)>;

	static std::shared_ptr<FakeDatagramNetwork> create() { return std::shared_ptr<FakeDatagramNetwork>(new FakeDatagramNetwork()); }

	// Sockets created by this factory behave like sockets of a host with the given address (in a /24 subnet)
	DatagramSocketFactory						factory(std::string_view hostIp) { return factory(std::vector<Interface>{{.ip = ipv4(hostIp)}}); }

	// ... or of a host with several interfaces
	DatagramSocketFactory						factory(std::vector<Interface> interfaces);

	void										setProfile(const LinkProfile &profile)
	{
		std::lock_guard<std::mutex> lock(mMutex);
		mProfile = profile;
		mRandom.seed(profile.seed);
	}

	// Sending to a host that is down fails like it does when nobody answers for its address
	void setHostDown(std::string_view hostIp, const bool down)
	{
		std::lock_guard<std::mutex> lock(mMutex);

		if (down)
			mDownHosts.push_back(ipv4(hostIp));
		else
			std::erase(mDownHosts, ipv4(hostIp));
	}

	// Every send of that host's sockets fails with the error
	void setSendError(std::string_view hostIp, const std::optional<SocketError> error)
	{
		std::lock_guard<std::mutex> lock(mMutex);

		if (error)
			mSendErrors[ipv4(hostIp)] = *error;
		else
			mSendErrors.erase(ipv4(hostIp));
	}

	// Delivers a datagram as if a socket at `from` had sent it: what someone who forges its sender address can do
	void inject(const SocketAddress &from, const SocketAddress &to, const std::span<const uint8_t> data)
	{
		std::vector<std::shared_ptr<Inbox>> targets;
		{
			std::lock_guard<std::mutex> lock(mMutex);
			targets = targetsOf(from.ip, to);
		}

		for (const auto &target : targets)
			target->add({.at = mTime->now(), .payload = std::vector<uint8_t>(data.begin(), data.end()), .from = from});
	}

	// Sees every datagram a socket accepted, before the network loses or delays it. Called with the network locked.
	void setTap(Tap tap)
	{
		std::lock_guard<std::mutex> lock(mMutex);
		mTap = std::move(tap);
	}

	size_t deliveredCount() const
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return mDelivered;
	}

	// Datagrams the sockets of that host accepted
	size_t sentBy(std::string_view hostIp) const
	{
		std::lock_guard<std::mutex> lock(mMutex);
		const auto					it = mSentBy.find(ipv4(hostIp));
		return it != mSentBy.end() ? it->second : 0;
	}

	// Sends that were refused because the socket's queue was full
	size_t blockedSends() const
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return mBlockedSends;
	}

	// --- Virtual time ----------------------------------------------------------

	void					 useVirtualTime(const TimePoint start) { mTime->set(start); }
	void					 advanceTo(const TimePoint now) { mTime->set(now); }
	TimePoint				 now() const { return mTime->now(); }

	// When the next datagram arrives that no socket read yet. Now or earlier: it is waiting. With `after`, only
	// arrivals later than that count.
	std::optional<TimePoint> nextArrival(std::optional<TimePoint> after = std::nullopt) const;

	// Datagrams sockets took out of the network so far
	size_t					 receivedCount() const { return mTime->received.load(); }

	// Whether a socket was interrupted since the last call: its owner has something to do
	bool					 takeInterrupts();

private:
	friend class FakeDatagramSocket;

	struct Time
	{
		TimePoint now() const
		{
			const auto ticks = virtualNow.load();
			return ticks != 0 ? TimePoint(Clock::duration(ticks)) : Clock::now();
		}

		void					set(const TimePoint time) { virtualNow.store(time.time_since_epoch().count()); }
		bool					isVirtual() const { return virtualNow.load() != 0; }

		std::atomic<Clock::rep> virtualNow{0}; // 0: the real clock
		std::atomic<size_t>		received{0};
	};

	struct Arrival
	{
		TimePoint			 at;
		std::vector<uint8_t> payload;
		SocketAddress		 from;
	};

	// One socket reads from an inbox, like one thread reads from a real socket
	struct Inbox
	{
		// The outcome of a wait: what to do with whatever is in the inbox now
		enum class State
		{
			Readable,
			Shutdown,
			Interrupted,
			TimedOut,
		};

		explicit Inbox(std::shared_ptr<Time> time) : time(std::move(time)) {}

		// Waits like the real socket: as precisely as the platform's deadline timer, so tests with short protocol
		// timers do not run on the scheduler tick of the test machine
		State waitUntilReadable(const std::chrono::microseconds timeout)
		{
			const auto deadline = time->now() + timeout;

			while (true)
			{
				auto wake = deadline;

				{
					std::lock_guard<std::mutex> lock(mutex);

					if (shutdown)
						return State::Shutdown;

					// Like the real socket: a waiting datagram comes first, the interrupt then ends the next wait
					if (!queue.empty() && queue.front().at <= time->now())
						return State::Readable;

					if (std::exchange(interrupted, false))
						return State::Interrupted;

					if (!queue.empty())
						wake = std::min(wake, queue.front().at);
				}

				// Virtual time only moves between the steps of whoever owns it: there is nothing to wait for
				if (time->isVirtual() || time->now() >= deadline)
					return State::TimedOut;

				changed->waitUntil(wake);
			}
		}

		void add(Arrival arrival)
		{
			{
				std::lock_guard<std::mutex> lock(mutex);

				// In order of arrival; what arrives at the same time stays in the order it was sent
				const auto					position = std::ranges::upper_bound(queue, arrival.at, {}, &Arrival::at);
				queue.insert(position, std::move(arrival));
			}
			changed->wake();
		}

		std::shared_ptr<Time>					 time;
		std::mutex								 mutex;
		std::unique_ptr<netlink::IDeadlineTimer> changed{netlink::makeDeadlineTimer()}; // woken with every change below
		std::deque<Arrival>						 queue;
		bool									 shutdown{false};
		bool									 interrupted{false};
		bool									 interruptSeen{false}; // by takeInterrupts()
	};

	struct Binding
	{
		std::weak_ptr<Inbox>  inbox;
		size_t				  host{0}; // index into mHosts
		IPv4Address			  boundIp;
		uint16_t			  port{0};
		bool				  reuse{false};
		std::deque<TimePoint> departures; // when the datagrams still waiting for the wire leave it
	};

	FakeDatagramNetwork() = default;

	Result<std::unique_ptr<IDatagramSocket>> bind(size_t host, const SocketAddress &local, const BindOptions &options);

	Result<size_t>							 send(const Inbox *sender, const SocketAddress &to, std::span<const uint8_t> data);

	bool									 roll(const double probability) { return probability > 0.0 && std::uniform_real_distribution<double>(0.0, 1.0)(mRandom) < probability; }

	// The address a socket of that host sends from: its own if it is bound to one, otherwise that of the iface towards the destination
	IPv4Address								 sourceOf(const Binding &binding, const IPv4Address &destination) const
	{
		if (!binding.boundIp.isUnspecified())
			return binding.boundIp;

		const auto &interfaces = mHosts[binding.host];
		const auto	towards	   = std::ranges::find_if(interfaces, [&](const Interface &iface) { return iface.reaches(destination) || iface.broadcast() == destination; });
		return towards != interfaces.end() ? towards->ip : interfaces.front().ip;
	}

	std::vector<std::shared_ptr<Inbox>> targetsOf(const IPv4Address &source, const SocketAddress &to) const
	{
		std::vector<std::shared_ptr<Inbox>> targets;

		// The subnet a broadcast goes to: the one of the iface it leaves from
		std::optional<Interface>			subnet;
		for (const auto &interfaces : mHosts)
		{
			for (const auto &iface : interfaces)
			{
				if (iface.ip == source && (to.ip.isBroadcast() || iface.broadcast() == to.ip))
					subnet = iface;
			}
		}

		for (const auto &binding : mBindings)
		{
			auto inbox = binding.inbox.lock();
			if (!inbox || binding.port != to.port)
				continue;

			const auto &interfaces = mHosts[binding.host];

			if (subnet)
			{
				const bool onSubnet = std::ranges::any_of(interfaces, [&](const Interface &iface) { return subnet->reaches(iface.ip); });

				if (onSubnet && (binding.boundIp.isUnspecified() || subnet->reaches(binding.boundIp)))
					targets.push_back(std::move(inbox));

				continue;
			}

			const bool owned = std::ranges::any_of(interfaces, [&](const Interface &iface) { return iface.ip == to.ip; });

			// Several sockets may share the port: like a real host, only one of them gets a unicast datagram
			if (owned && (binding.boundIp.isUnspecified() || binding.boundIp == to.ip))
			{
				targets.push_back(std::move(inbox));
				break;
			}
		}

		return targets;
	}

	std::shared_ptr<Time>				mTime = std::make_shared<Time>();

	mutable std::mutex					mMutex;
	std::vector<std::vector<Interface>> mHosts;
	std::vector<Binding>				mBindings;
	LinkProfile							mProfile;
	std::mt19937						mRandom{1};
	std::vector<IPv4Address>			mDownHosts;
	std::map<IPv4Address, SocketError>	mSendErrors; // by the first address of the sending host
	std::map<IPv4Address, size_t>		mSentBy;
	Tap									mTap;
	uint16_t							mNextEphemeralPort{50000};
	size_t								mDelivered{0};
	size_t								mBlockedSends{0};
};


class FakeDatagramSocket final : public IDatagramSocket
{
public:
	FakeDatagramSocket(std::weak_ptr<FakeDatagramNetwork> network, std::shared_ptr<FakeDatagramNetwork::Inbox> inbox, SocketAddress local)
		: mNetwork(std::move(network)), mInbox(std::move(inbox)), mLocal(std::move(local))
	{
	}

	Result<size_t> sendTo(const SocketAddress &destination, std::span<const uint8_t> data) override
	{
		auto network = mNetwork.lock();
		if (!network)
			return std::unexpected(SocketError::NetworkUnreachable);

		return network->send(mInbox.get(), destination, data);
	}

	Result<Datagram> receiveFrom(std::span<uint8_t> buffer, std::chrono::microseconds timeout) override
	{
		// Like the real socket: reading comes first, and a read that does not wait leaves a pending interrupt alone
		if (auto datagram = take(buffer); datagram || datagram.error() != SocketError::WouldBlock)
			return datagram;

		if (timeout <= std::chrono::microseconds::zero())
			return std::unexpected(SocketError::Timeout);

		if (auto ready = waitReadable(timeout); !ready)
			return std::unexpected(ready.error());

		// Taken by another reader in the meantime
		auto datagram = take(buffer);
		return datagram || datagram.error() != SocketError::WouldBlock ? datagram : std::unexpected(SocketError::Timeout);
	}

	Result<void> waitReadable(std::chrono::microseconds timeout) override
	{
		using State = FakeDatagramNetwork::Inbox::State;

		switch (mInbox->waitUntilReadable(timeout))
		{
		case State::Readable: return {};
		case State::Shutdown: return std::unexpected(SocketError::Closed);
		case State::Interrupted: return std::unexpected(SocketError::Cancelled);
		case State::TimedOut: break;
		}

		return std::unexpected(SocketError::Timeout);
	}

	void interrupt() override
	{
		{
			std::lock_guard<std::mutex> lock(mInbox->mutex);
			mInbox->interrupted	  = true;
			mInbox->interruptSeen = false;
		}
		mInbox->changed->wake();
	}

	SocketAddress localAddress() const override { return mLocal; }

	void		  shutdown() override
	{
		{
			std::lock_guard<std::mutex> lock(mInbox->mutex);
			mInbox->shutdown = true;
		}
		mInbox->changed->wake();
	}

private:
	// The oldest datagram that arrived, SocketError::WouldBlock if there is none
	Result<Datagram> take(std::span<uint8_t> buffer)
	{
		std::lock_guard<std::mutex> lock(mInbox->mutex);

		if (mInbox->shutdown)
			return std::unexpected(SocketError::Closed);

		if (mInbox->queue.empty() || mInbox->queue.front().at > mInbox->time->now())
			return std::unexpected(SocketError::WouldBlock);

		auto arrival = std::move(mInbox->queue.front());
		mInbox->queue.pop_front();
		++mInbox->time->received;

		const size_t size = std::min(arrival.payload.size(), buffer.size());
		std::memcpy(buffer.data(), arrival.payload.data(), size);
		return Datagram{size, arrival.from};
	}

	std::weak_ptr<FakeDatagramNetwork>			mNetwork;
	std::shared_ptr<FakeDatagramNetwork::Inbox> mInbox;
	SocketAddress								mLocal;
};


inline Result<std::unique_ptr<IDatagramSocket>> FakeDatagramNetwork::bind(const size_t host, const SocketAddress &local, const BindOptions &options)
{
	std::lock_guard<std::mutex> lock(mMutex);

	std::erase_if(mBindings, [](const Binding &binding) { return binding.inbox.expired(); });

	uint16_t port = local.port;

	if (port == 0)
	{
		port = mNextEphemeralPort++;
	}
	else
	{
		for (const auto &binding : mBindings)
		{
			const bool sameAddress = binding.boundIp.isUnspecified() || local.ip.isUnspecified() || binding.boundIp == local.ip;

			if (binding.host == host && binding.port == port && sameAddress && !(binding.reuse && options.reuseAddress))
				return std::unexpected(SocketError::AddressInUse);
		}
	}

	auto inbox = std::make_shared<Inbox>(mTime);
	mBindings.push_back({.inbox = inbox, .host = host, .boundIp = local.ip, .port = port, .reuse = options.reuseAddress, .departures = {}});

	return std::make_unique<FakeDatagramSocket>(weak_from_this(), inbox, SocketAddress{local.ip, port});
}


inline Result<size_t> FakeDatagramNetwork::send(const Inbox *sender, const SocketAddress &to, const std::span<const uint8_t> data)
{
	std::vector<std::shared_ptr<Inbox>> targets;
	Arrival								arrival;

	{
		std::lock_guard<std::mutex> lock(mMutex);

		const auto					binding = std::ranges::find_if(mBindings, [&](const Binding &candidate) { return candidate.inbox.lock().get() == sender; });
		if (binding == mBindings.end())
			return std::unexpected(SocketError::Closed);

		const IPv4Address host = mHosts[binding->host].front().ip;

		if (const auto error = mSendErrors.find(host); error != mSendErrors.end())
			return std::unexpected(error->second);

		if (std::ranges::find(mDownHosts, to.ip) != mDownHosts.end())
			return std::unexpected(SocketError::NetworkUnreachable);

		const TimePoint now		  = mTime->now();
		TimePoint		departure = now;

		if (mProfile.bandwidth > 0)
		{
			auto &waiting = binding->departures;

			while (!waiting.empty() && waiting.front() <= now)
				waiting.pop_front();

			if (mProfile.queueLimit > 0 && waiting.size() >= mProfile.queueLimit)
			{
				if (mProfile.blockWhenFull)
				{
					++mBlockedSends;
					return std::unexpected(SocketError::WouldBlock);
				}

				return data.size(); // accepted, and dropped at the end of the queue
			}

			const auto onTheWire = std::chrono::nanoseconds(data.size() * 1'000'000'000ull / mProfile.bandwidth);
			departure			 = (waiting.empty() ? now : waiting.back()) + std::chrono::duration_cast<Clock::duration>(onTheWire);
			waiting.push_back(departure);
		}

		const SocketAddress from{sourceOf(*binding, to.ip), binding->port};

		++mSentBy[host];

		if (mTap)
			mTap(from, to, data);

		if (roll(mProfile.lossRate))
			return data.size();

		arrival.at		= departure + mProfile.latency + (roll(mProfile.reorderRate) ? mProfile.reorderDelay : std::chrono::microseconds{0});
		arrival.payload = std::vector<uint8_t>(data.begin(), data.end());
		arrival.from	= from;

		targets			= targetsOf(from.ip, to);
		mDelivered += targets.size();
	}

	for (size_t i = 0; i < targets.size(); ++i)
		targets[i]->add(i + 1 < targets.size() ? arrival : std::move(arrival));

	return data.size();
}


inline std::optional<TimePoint> FakeDatagramNetwork::nextArrival(const std::optional<TimePoint> after) const
{
	std::lock_guard<std::mutex> lock(mMutex);
	std::optional<TimePoint>	next;

	for (const auto &binding : mBindings)
	{
		const auto inbox = binding.inbox.lock();
		if (!inbox)
			continue;

		std::lock_guard<std::mutex> inboxLock(inbox->mutex);

		const auto					first = std::ranges::find_if(inbox->queue, [&](const Arrival &arrival) { return !after || arrival.at > *after; });

		if (first != inbox->queue.end() && (!next || first->at < *next))
			next = first->at;
	}

	return next;
}


inline bool FakeDatagramNetwork::takeInterrupts()
{
	std::lock_guard<std::mutex> lock(mMutex);
	bool						any = false;

	for (const auto &binding : mBindings)
	{
		const auto inbox = binding.inbox.lock();
		if (!inbox)
			continue;

		std::lock_guard<std::mutex> inboxLock(inbox->mutex);

		if (inbox->interrupted && !std::exchange(inbox->interruptSeen, true))
			any = true;
	}

	return any;
}


inline DatagramSocketFactory FakeDatagramNetwork::factory(std::vector<Interface> interfaces)
{
	size_t host = 0;
	{
		std::lock_guard<std::mutex> lock(mMutex);

		// The same first address is the same host
		const auto					known = std::ranges::find_if(mHosts, [&](const auto &existing) { return existing.front().ip == interfaces.front().ip; });
		host							  = static_cast<size_t>(known - mHosts.begin());

		if (known == mHosts.end())
			mHosts.push_back(std::move(interfaces));
	}

	return [network = shared_from_this(), host](const SocketAddress &local, const BindOptions &options) { return network->bind(host, local, options); };
}

} // namespace FakeNet
