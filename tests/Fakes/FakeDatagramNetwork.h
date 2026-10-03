/*
  ==============================================================================
	Module:         FakeDatagramNetwork
	Description:    In-memory datagram network for deterministic tests of
					datagram based services (no real ports, no OS sockets).
  ==============================================================================
*/

#pragma once

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
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


class FakeDatagramNetwork : public std::enable_shared_from_this<FakeDatagramNetwork>
{
public:
	static std::shared_ptr<FakeDatagramNetwork> create() { return std::shared_ptr<FakeDatagramNetwork>(new FakeDatagramNetwork()); }

	// Sockets created by this factory behave like sockets of a host with the given IP
	DatagramSocketFactory						factory(std::string_view hostIp);

	size_t										deliveredCount() const
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return mDelivered;
	}

private:
	friend class FakeDatagramSocket;

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

		// Waits like the real socket: as precisely as the platform's deadline timer, so tests with short protocol
		// timers do not run on the scheduler tick of the test machine
		State waitUntilReadable(const std::chrono::microseconds timeout)
		{
			const auto deadline = netlink::IDeadlineTimer::Clock::now() + timeout;

			while (true)
			{
				{
					std::lock_guard<std::mutex> lock(mutex);

					if (shutdown)
						return State::Shutdown;

					// Like the real socket: a waiting datagram comes first, the interrupt then ends the next wait
					if (!queue.empty())
						return State::Readable;

					if (std::exchange(interrupted, false))
						return State::Interrupted;
				}

				if (netlink::IDeadlineTimer::Clock::now() >= deadline)
					return State::TimedOut;

				changed->waitUntil(deadline);
			}
		}

		std::mutex												   mutex;
		std::unique_ptr<netlink::IDeadlineTimer>				   changed{netlink::makeDeadlineTimer()}; // woken with every change below
		std::deque<std::pair<std::vector<uint8_t>, SocketAddress>> queue;
		bool													   shutdown{false};
		bool													   interrupted{false};
	};

	struct Binding
	{
		std::weak_ptr<Inbox> inbox;
		IPv4Address			 hostIp;
		IPv4Address			 boundIp;
		uint16_t			 port{0};
		bool				 reuse{false};
	};

	FakeDatagramNetwork() = default;

	Result<std::unique_ptr<IDatagramSocket>> bind(const IPv4Address &hostIp, const SocketAddress &local, const BindOptions &options);

	void									 deliver(const SocketAddress &from, const SocketAddress &to, std::span<const uint8_t> data)
	{
		std::vector<std::shared_ptr<Inbox>> targets;

		{
			std::lock_guard<std::mutex> lock(mMutex);

			for (const auto &binding : mBindings)
			{
				auto inbox = binding.inbox.lock();
				if (!inbox || binding.port != to.port)
					continue;

				const bool matches = to.ip.isBroadcast() || to.ip == binding.boundIp || to.ip == binding.hostIp;
				if (matches)
					targets.push_back(std::move(inbox));
			}

			mDelivered += targets.size();
		}

		for (auto &inbox : targets)
		{
			{
				std::lock_guard<std::mutex> lock(inbox->mutex);
				inbox->queue.emplace_back(std::vector<uint8_t>(data.begin(), data.end()), from);
			}
			inbox->changed->wake();
		}
	}

	mutable std::mutex	 mMutex;
	std::vector<Binding> mBindings;
	uint16_t			 mNextEphemeralPort{50000};
	size_t				 mDelivered{0};
};


class FakeDatagramSocket final : public IDatagramSocket
{
public:
	FakeDatagramSocket(std::weak_ptr<FakeDatagramNetwork> network, std::shared_ptr<FakeDatagramNetwork::Inbox> inbox, IPv4Address hostIp, SocketAddress local)
		: mNetwork(std::move(network)), mInbox(std::move(inbox)), mHostIp(hostIp), mLocal(std::move(local))
	{
	}

	Result<size_t> sendTo(const SocketAddress &destination, std::span<const uint8_t> data) override
	{
		auto network = mNetwork.lock();
		if (!network)
			return std::unexpected(SocketError::NetworkUnreachable);

		network->deliver({mHostIp, mLocal.port}, destination, data);
		return data.size();
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
			mInbox->interrupted = true;
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
	// The oldest waiting datagram, SocketError::WouldBlock if there is none
	Result<Datagram> take(std::span<uint8_t> buffer)
	{
		std::lock_guard<std::mutex> lock(mInbox->mutex);

		if (mInbox->shutdown)
			return std::unexpected(SocketError::Closed);

		if (mInbox->queue.empty())
			return std::unexpected(SocketError::WouldBlock);

		auto [payload, from] = std::move(mInbox->queue.front());
		mInbox->queue.pop_front();

		const size_t size = std::min(payload.size(), buffer.size());
		std::memcpy(buffer.data(), payload.data(), size);
		return Datagram{size, from};
	}

	std::weak_ptr<FakeDatagramNetwork>			mNetwork;
	std::shared_ptr<FakeDatagramNetwork::Inbox> mInbox;
	IPv4Address									mHostIp;
	SocketAddress								mLocal;
};


inline Result<std::unique_ptr<IDatagramSocket>> FakeDatagramNetwork::bind(const IPv4Address &hostIp, const SocketAddress &local, const BindOptions &options)
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
			if (binding.hostIp == hostIp && binding.port == port && !(binding.reuse && options.reuseAddress))
				return std::unexpected(SocketError::AddressInUse);
		}
	}

	auto inbox = std::make_shared<Inbox>();
	mBindings.push_back({inbox, hostIp, local.ip, port, options.reuseAddress});

	return std::make_unique<FakeDatagramSocket>(weak_from_this(), inbox, hostIp, SocketAddress{local.ip, port});
}


inline DatagramSocketFactory FakeDatagramNetwork::factory(std::string_view hostIp)
{
	return [network = shared_from_this(), host = ipv4(hostIp)](const SocketAddress &local, const BindOptions &options) { return network->bind(host, local, options); };
}

} // namespace FakeNet
