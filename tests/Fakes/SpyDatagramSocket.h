/*
  ==============================================================================
	Module:         SpyDatagramSocket
	Description:    Decorator that records how a socket is used: which threads
					send through it, and how often it is sent on and waited for
  ==============================================================================
*/

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

#include "Socket/IDatagramSocket.h"


namespace FakeNet
{

using namespace netlink::net;


// Shared by the test and the sockets it spies on
struct SocketUsage
{
	std::set<std::thread::id> sendingThreads()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return senders;
	}

	void noteSend()
	{
		++sends;
		std::lock_guard<std::mutex> lock(mutex);
		senders.insert(std::this_thread::get_id());
	}

	std::atomic<size_t>		  sends{0};
	std::atomic<bool>		  throwOnReceive{false}; // the next receive throws, like a bug in the code that runs on the socket's thread

	std::mutex				  mutex;
	std::set<std::thread::id> senders;
};


class SpyDatagramSocket final : public IDatagramSocket
{
public:
	SpyDatagramSocket(std::unique_ptr<IDatagramSocket> inner, std::shared_ptr<SocketUsage> usage) : mInner(std::move(inner)), mUsage(std::move(usage)) {}

	static DatagramSocketFactory wrap(DatagramSocketFactory inner, std::shared_ptr<SocketUsage> usage)
	{
		return [inner = std::move(inner), usage = std::move(usage)](const SocketAddress &local, const BindOptions &options) -> Result<std::unique_ptr<IDatagramSocket>>
		{
			auto socket = inner(local, options);
			if (!socket)
				return std::unexpected(socket.error());

			return std::make_unique<SpyDatagramSocket>(std::move(*socket), usage);
		};
	}

	Result<size_t> sendTo(const SocketAddress &destination, std::span<const uint8_t> data) override
	{
		mUsage->noteSend();
		return mInner->sendTo(destination, data);
	}

	Result<size_t> sendParts(const SocketAddress &destination, std::span<const uint8_t> head, std::span<const uint8_t> body) override
	{
		mUsage->noteSend();
		return mInner->sendParts(destination, head, body);
	}

	Result<Datagram> receiveFrom(std::span<uint8_t> buffer, std::chrono::microseconds timeout) override
	{
		if (mUsage->throwOnReceive.exchange(false))
			throw std::runtime_error("receive failed unexpectedly");

		return mInner->receiveFrom(buffer, timeout);
	}

	Result<void>  waitReadable(std::chrono::microseconds timeout) override { return mInner->waitReadable(timeout); }

	void		  interrupt() override { mInner->interrupt(); }

	SocketAddress localAddress() const override { return mInner->localAddress(); }

	void		  shutdown() override { mInner->shutdown(); }

private:
	std::unique_ptr<IDatagramSocket> mInner;
	std::shared_ptr<SocketUsage>	 mUsage;
};

} // namespace FakeNet
