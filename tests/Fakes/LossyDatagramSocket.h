/*
  ==============================================================================
	Module:         LossyDatagramSocket
	Description:    Decorator that simulates an unreliable network (drop,
					duplicate, reorder) with a deterministic seed. Foundation
					for testing reliability layers on top of datagrams.
  ==============================================================================
*/

#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <vector>

#include "Socket/IDatagramSocket.h"


namespace FakeNet
{

using namespace netlink::net;


struct LossProfile
{
	double	 dropRate{0.0};		 // probability a datagram disappears
	double	 duplicateRate{0.0}; // probability a datagram is delivered twice
	double	 reorderRate{0.0};	 // probability a datagram is held back and delivered after the next one
	uint32_t seed{42};
};


class LossyDatagramSocket final : public IDatagramSocket
{
public:
	LossyDatagramSocket(std::unique_ptr<IDatagramSocket> inner, LossProfile profile) : mInner(std::move(inner)), mProfile(profile), mRandom(profile.seed) {}

	static DatagramSocketFactory wrap(DatagramSocketFactory inner, LossProfile profile)
	{
		return [inner = std::move(inner), profile](const SocketAddress &local, const BindOptions &options) -> Result<std::unique_ptr<IDatagramSocket>>
		{
			auto socket = inner(local, options);
			if (!socket)
				return std::unexpected(socket.error());

			return std::make_unique<LossyDatagramSocket>(std::move(*socket), profile);
		};
	}

	Result<size_t> sendTo(const SocketAddress &destination, std::span<const uint8_t> data) override
	{
		std::lock_guard<std::mutex> lock(mMutex);

		if (roll(mProfile.dropRate))
			return data.size(); // "sent", but lost on the way

		if (!mHeld && roll(mProfile.reorderRate))
		{
			mHeld = Held{destination, std::vector<uint8_t>(data.begin(), data.end())};
			return data.size();
		}

		auto result = mInner->sendTo(destination, data);

		if (result && roll(mProfile.duplicateRate))
			static_cast<void>(mInner->sendTo(destination, data));

		if (mHeld)
		{
			static_cast<void>(mInner->sendTo(mHeld->destination, mHeld->payload));
			mHeld.reset();
		}

		return result;
	}

	Result<Datagram> receiveFrom(std::span<uint8_t> buffer, std::chrono::milliseconds timeout) override { return mInner->receiveFrom(buffer, timeout); }

	SocketAddress	 localAddress() const override { return mInner->localAddress(); }

	void			 shutdown() override { mInner->shutdown(); }

private:
	struct Held
	{
		SocketAddress		 destination;
		std::vector<uint8_t> payload;
	};

	bool roll(double probability) { return probability > 0.0 && std::uniform_real_distribution<double>(0.0, 1.0)(mRandom) < probability; }

	std::unique_ptr<IDatagramSocket> mInner;
	LossProfile						 mProfile;
	std::mt19937					 mRandom;
	std::optional<Held>				 mHeld;
	std::mutex						 mMutex;
};

} // namespace FakeNet
