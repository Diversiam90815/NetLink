/*
  ==============================================================================
	Module:         FakeDatagramNetwork
	Description:    In-memory datagram network for deterministic tests of
					datagram based services (no real ports, no OS sockets).
  ==============================================================================
*/

#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Socket/IDatagramSocket.h"


namespace FakeNet
{

using namespace netlink::net;

inline constexpr const char *BroadcastAddress = "255.255.255.255";


class FakeDatagramNetwork : public std::enable_shared_from_this<FakeDatagramNetwork>
{
public:
	static std::shared_ptr<FakeDatagramNetwork> create() { return std::shared_ptr<FakeDatagramNetwork>(new FakeDatagramNetwork()); }

	// Sockets created by this factory behave like sockets of a host with the given IP
	DatagramSocketFactory						factory(const std::string &hostIp);

	size_t										deliveredCount() const
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return mDelivered;
	}

private:
	friend class FakeDatagramSocket;

	struct Inbox
	{
		std::mutex											   mutex;
		std::condition_variable								   cv;
		std::deque<std::pair<std::vector<uint8_t>, SocketAddress>> queue;
		bool												   shutdown{false};
	};

	struct Binding
	{
		std::weak_ptr<Inbox> inbox;
		std::string			 hostIp;
		std::string			 boundIp;
		uint16_t			 port{0};
		bool				 reuse{false};
	};

	FakeDatagramNetwork() = default;

	Result<std::unique_ptr<IDatagramSocket>> bind(const std::string &hostIp, const SocketAddress &local, const BindOptions &options);

	void deliver(const SocketAddress &from, const SocketAddress &to, std::span<const uint8_t> data)
	{
		std::vector<std::shared_ptr<Inbox>> targets;

		{
			std::lock_guard<std::mutex> lock(mMutex);

			for (const auto &binding : mBindings)
			{
				auto inbox = binding.inbox.lock();
				if (!inbox || binding.port != to.port)
					continue;

				const bool matches = to.ip == BroadcastAddress || to.ip == binding.boundIp || to.ip == binding.hostIp;
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
			inbox->cv.notify_all();
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
	FakeDatagramSocket(std::weak_ptr<FakeDatagramNetwork> network, std::shared_ptr<FakeDatagramNetwork::Inbox> inbox, std::string hostIp, SocketAddress local)
		: mNetwork(std::move(network)), mInbox(std::move(inbox)), mHostIp(std::move(hostIp)), mLocal(std::move(local))
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

	Result<Datagram> receiveFrom(std::span<uint8_t> buffer, std::chrono::milliseconds timeout) override
	{
		std::unique_lock<std::mutex> lock(mInbox->mutex);

		if (!mInbox->cv.wait_for(lock, timeout, [this] { return !mInbox->queue.empty() || mInbox->shutdown; }))
			return std::unexpected(SocketError::Timeout);

		if (mInbox->shutdown)
			return std::unexpected(SocketError::Closed);

		auto [payload, from] = std::move(mInbox->queue.front());
		mInbox->queue.pop_front();

		const size_t size	 = std::min(payload.size(), buffer.size());
		std::memcpy(buffer.data(), payload.data(), size);
		return Datagram{size, from};
	}

	SocketAddress localAddress() const override { return mLocal; }

	void		  shutdown() override
	{
		{
			std::lock_guard<std::mutex> lock(mInbox->mutex);
			mInbox->shutdown = true;
		}
		mInbox->cv.notify_all();
	}

private:
	std::weak_ptr<FakeDatagramNetwork>			 mNetwork;
	std::shared_ptr<FakeDatagramNetwork::Inbox> mInbox;
	std::string									 mHostIp;
	SocketAddress								 mLocal;
};


inline Result<std::unique_ptr<IDatagramSocket>> FakeDatagramNetwork::bind(const std::string &hostIp, const SocketAddress &local, const BindOptions &options)
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
	mBindings.push_back({inbox, hostIp, local.ip.empty() ? "0.0.0.0" : local.ip, port, options.reuseAddress});

	return std::make_unique<FakeDatagramSocket>(weak_from_this(), inbox, hostIp, SocketAddress{local.ip.empty() ? "0.0.0.0" : local.ip, port});
}


inline DatagramSocketFactory FakeDatagramNetwork::factory(const std::string &hostIp)
{
	return [network = shared_from_this(), hostIp](const SocketAddress &local, const BindOptions &options) { return network->bind(hostIp, local, options); };
}

} // namespace FakeNet
