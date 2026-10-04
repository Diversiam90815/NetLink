/*
  ==============================================================================
	Module:         PeerChannel
	Description:    The dedicated UDP socket all peer-to-peer traffic runs on
  ==============================================================================
*/

#include "PeerChannel.h"

#include <algorithm>
#include <iterator>
#include <ranges>
#include <thread>

#include "TransportConstants.h"
#include "NetLinkLog.h"
#include "Socket/UdpSocket.h"

using json = nlohmann::json;
using netlink::channel::Lane;
using netlink::channel::Mailbox;
using netlink::channel::SendScheduler;


namespace
{

// The longest the I/O thread waits without anything to do
constexpr auto	MaxWait = std::chrono::hours{1};

constexpr uint8_t bitOf(const Lane lane)
{
	return static_cast<uint8_t>(1u << std::to_underlying(lane));
}

// Hands a link the messages that wait for its peer in the mailbox
class MailboxSource final : public netlink::channel::MessageSource
{
public:
	MailboxSource(Mailbox &mailbox, const netlink::net::SocketAddress &peer) : mMailbox(mailbox), mPeer(peer) {}

	std::optional<netlink::channel::OutboundMessage> next(const Lane lane) override
	{
		auto mail = mMailbox.take(mPeer, lane);
		if (!mail)
			return std::nullopt;

		return netlink::channel::OutboundMessage{.tag = mail->tag, .body = std::move(mail->body)};
	}

private:
	Mailbox							  &mMailbox;
	const netlink::net::SocketAddress &mPeer;
};

template <typename TimePoint>
std::optional<TimePoint> earlier(const std::optional<TimePoint> a, const std::optional<TimePoint> b)
{
	if (!a || !b)
		return a ? a : b;

	return std::min(*a, *b);
}

} // namespace


netlink::PeerChannel::PeerChannel(net::DatagramSocketFactory socketFactory, const PeerChannelConfig &config, TaskQueue *applicationQueue)
	: mSocketFactory(socketFactory ? std::move(socketFactory) : net::UdpSocket::factory()), mPendingConfig(config), mApplicationQueue(applicationQueue),
	  mMailbox([this] { wakeIoThread(); }), mConfig(config), mHeartbeat(config.timings), mScheduler(config.maxSendRate), mReceiveBuffer(internal::PackageBufferSize)
{
	mMailbox.setQueueBytes(config.sendQueueBytes);
	mDeliveryBacklog->onDrained = [this] { mMailbox.post(Mailbox::Command::ResumeReceiving); };
}


netlink::PeerChannel::~PeerChannel()
{
	deinit();

	// Payload that is still waiting somewhere must not call into a channel that is gone
	std::lock_guard<std::mutex> lock(mDeliveryBacklog->mutex);
	mDeliveryBacklog->onDrained = nullptr;
}


bool netlink::PeerChannel::init(const std::string &localComputerName)
{
	if (localComputerName.empty())
		return false;

	{
		std::lock_guard<std::mutex> lock(mSocketMutex);
		mLocalComputerName = localComputerName;
	}

	// Binding deferred until setLocalIPv4() is called
	mInitialized.store(true);
	return true;
}


void netlink::PeerChannel::deinit()
{
	stop();

	{
		std::lock_guard<std::mutex> lock(mSocketMutex);

		if (mSocket)
			mSocket->shutdown();

		mSocket.reset();
	}

	// The I/O thread is gone: what it owned and what was still meant for it is discarded here
	forgetLinks();
	mMailbox.reset();
	mMailbox.drain(mWork);

	mBoundPort.store(0);
	mInitialized.store(false);
}


void netlink::PeerChannel::setConfig(const PeerChannelConfig &config)
{
	{
		std::lock_guard<std::mutex> lock(mSocketMutex);
		mPendingConfig = config;
	}

	mMailbox.setQueueBytes(config.sendQueueBytes);
	mMailbox.post(Mailbox::Command::Reconfigure);
}


void netlink::PeerChannel::setLocalIPv4(const net::IPv4Address &localIPv4)
{
	if (localIPv4.isUnspecified())
		return;

	uint32_t sendRate = 0;
	{
		std::lock_guard<std::mutex> lock(mSocketMutex);
		sendRate = mPendingConfig.maxSendRate;
	}

	// Room for one burst of the send budget, where the operating system needs to be asked for it
	const auto burst = static_cast<int>(SendScheduler::burstOf(sendRate > 0 ? sendRate : channel::DefaultMaxSendRate));
	auto	   socket =
		mSocketFactory({.ip = localIPv4, .port = 0}, {.receiveBufferSize = internal::ChannelReceiveBufferSize, .sendBufferSize = burst * internal::ChannelSendBufferPerDatagram});

	if (!socket)
	{
		NETLINK_LOG_ERROR("Failed to bind the peer channel socket to {}: {}", localIPv4.toString(), net::toString(socket.error()));
		return;
	}

	const int							  boundPort = (*socket)->localAddress().port;
	std::shared_ptr<net::IDatagramSocket> previous;

	{
		std::lock_guard<std::mutex> lock(mSocketMutex);
		previous   = std::exchange(mSocket, std::shared_ptr<net::IDatagramSocket>(std::move(*socket)));
		mLocalIPv4 = localIPv4;
	}

	if (previous)
		previous->shutdown();

	// Streams of the previous socket cannot continue on the new one
	resetLinks();
	triggerEvent();

	mBoundPort.store(boundPort);
	NETLINK_LOG_INFO("PeerChannel bound to {}:{}", localIPv4.toString(), boundPort);

	if (mOnSocketBound)
		mOnSocketBound(boundPort);
}


std::shared_ptr<netlink::net::IDatagramSocket> netlink::PeerChannel::socket() const
{
	std::lock_guard<std::mutex> lock(mSocketMutex);
	return mSocket;
}


void netlink::PeerChannel::resetLinks()
{
	mMailbox.reset();
}


void netlink::PeerChannel::wakeIoThread()
{
	if (const auto socket = this->socket())
		socket->interrupt();
	else
		triggerEvent();
}


// ---------------------------------------------------------------------------
// Peer registry
// ---------------------------------------------------------------------------

void netlink::PeerChannel::registerPeer(const std::string &displayName, const net::IPv4Address &ipv4, const int channelPort)
{
	const PeerEndpoint endpoint{.IPv4 = ipv4, .channelPort = channelPort};
	PeerEndpoint	   previous;
	bool			   previousInUse = false;

	{
		std::lock_guard<std::mutex> lock(mPeerRegistryMutex);

		if (const auto it = mPeerRegistry.find(displayName); it != mPeerRegistry.end())
		{
			previous = it->second;
			forgetAddress(previous.address(), displayName);
		}

		mPeerRegistry[displayName]		   = endpoint;
		mNameByAddress[endpoint.address()] = displayName;
		previousInUse					   = previous.isValid() && mNameByAddress.contains(previous.address());
	}

	NETLINK_LOG_DEBUG("Registered peer {} -> {}:{}", displayName, ipv4.toString(), channelPort);

	// The peer moved to another socket (restart, adapter switch): its old stream is gone
	if (previous.isValid() && previous.address() != endpoint.address())
	{
		mMailbox.close(previous.address());

		if (previousInUse)
			mMailbox.open(previous.address());
	}

	mMailbox.open(endpoint.address());
}


void netlink::PeerChannel::unregisterPeer(const std::string &displayName)
{
	PeerEndpoint removed;
	bool		 stillInUse = false;

	{
		std::lock_guard<std::mutex> lock(mPeerRegistryMutex);

		if (const auto it = mPeerRegistry.find(displayName); it != mPeerRegistry.end())
		{
			removed = it->second;
			mPeerRegistry.erase(it);
			forgetAddress(removed.address(), displayName);
			stillInUse = mNameByAddress.contains(removed.address());
		}
	}

	if (removed.isValid())
	{
		mMailbox.close(removed.address());

		if (stillInUse)
			mMailbox.open(removed.address());
	}

	NETLINK_LOG_DEBUG("Unregistered peer {}", displayName);
}


netlink::PeerEndpoint netlink::PeerChannel::resolvePeer(const std::string &computerName) const
{
	std::lock_guard<std::mutex> lock(mPeerRegistryMutex);

	const auto					it = mPeerRegistry.find(computerName);
	if (it == mPeerRegistry.end())
	{
		NETLINK_LOG_WARNING("Cannot resolve peer {}: not registered", computerName);
		return {};
	}

	return it->second;
}


std::string netlink::PeerChannel::nameOf(const net::SocketAddress &address) const
{
	std::lock_guard<std::mutex> lock(mPeerRegistryMutex);

	const auto					it = mNameByAddress.find(address);
	return it != mNameByAddress.end() ? it->second : std::string{};
}


void netlink::PeerChannel::forgetAddress(const net::SocketAddress &address, const std::string &displayName)
{
	const auto it = mNameByAddress.find(address);
	if (it == mNameByAddress.end() || it->second != displayName)
		return;

	mNameByAddress.erase(it);

	// Another peer registered under the same address keeps resolving
	for (const auto &[name, endpoint] : mPeerRegistry)
	{
		if (name != displayName && endpoint.address() == address)
		{
			mNameByAddress[address] = name;
			return;
		}
	}
}


// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

bool netlink::PeerChannel::sendConnectRequest(const std::string &computerName)
{
	return sendSignal(computerName, SignalType::ConnectRequest);
}


bool netlink::PeerChannel::sendConnectAnswer(const std::string &computerName, const bool requestAccepted, const std::string &reason)
{
	return sendSignal(computerName, SignalType::ConnectAnswer, PayloadConnectAnswer{.accepted = requestAccepted, .reason = reason});
}


bool netlink::PeerChannel::sendDisconnect(const std::string &computerName)
{
	return sendSignal(computerName, SignalType::Disconnect);
}


bool netlink::PeerChannel::sendReadyFlag(const std::string &computerName, const bool ready)
{
	return sendSignal(computerName, SignalType::ReadyFlag, PayloadReadyFlag{ready});
}


bool netlink::PeerChannel::sendValidationRequest(const std::string &computerName, RemoteRequest request)
{
	return sendSignal(computerName, SignalType::ValidationRequest, PayloadValidationRequest{static_cast<uint8_t>(request)});
}


bool netlink::PeerChannel::sendSecretResponse(const std::string &computerName, const std::string &secret)
{
	return sendSignal(computerName, SignalType::SecretResponse, PayloadSecretResponse{secret});
}


bool netlink::PeerChannel::sendVersionResponse(const std::string &computerName, const std::string &version)
{
	return sendSignal(computerName, SignalType::VersionResponse, PayloadVersionResponse{version});
}


bool netlink::PeerChannel::sendValidationHandshake(const std::string &computerName)
{
	return sendSignal(computerName, SignalType::ValidationHandshake);
}


bool netlink::PeerChannel::sendSignal(const std::string &computerName, SignalType type, decltype(SignalPacket::payload) payload)
{
	auto peer = resolvePeer(computerName);
	if (!peer.isValid())
		return false;

	SignalPacket packet{};
	packet.signalType = type;
	packet.payload	  = std::move(payload);

	{
		std::lock_guard<std::mutex> lock(mSocketMutex);
		packet.senderName = mLocalComputerName;
	}

	const std::string encoded = json(packet).dump();
	const bool		  queued  = push(peer, Lane::Control, 0, std::vector<uint8_t>(encoded.begin(), encoded.end()));

	if (queued)
		NETLINK_LOG_DEBUG("Signal queued for {} (type={})", computerName, static_cast<int>(type));

	return queued;
}


bool netlink::PeerChannel::sendMessage(const std::string &computerName, const uint32_t type, std::span<const uint8_t> data, const DeliveryMode mode,
									   const std::chrono::milliseconds timeout)
{
	const auto peer = resolvePeer(computerName);
	if (!peer.isValid())
		return false;

	// The type travels as the tag of the message: the payload is copied once, into the buffer its fragments are sent from
	const Lane lane = mode == DeliveryMode::ReliableOrdered ? Lane::Reliable : Lane::Media;
	return push(peer, lane, type, std::vector<uint8_t>(data.begin(), data.end()), timeout);
}


bool netlink::PeerChannel::push(const PeerEndpoint &peer, const Lane lane, const uint32_t tag, std::vector<uint8_t> body, const std::chrono::milliseconds timeout)
{
	if (!mInitialized.load())
	{
		NETLINK_LOG_ERROR("PeerChannel not initialized -> cannot send.");
		return false;
	}

	if (!socket())
	{
		NETLINK_LOG_ERROR("PeerChannel not bound -> cannot send.");
		return false;
	}

	Mailbox::Mail mail{.tag = tag, .body = std::make_shared<const std::vector<uint8_t>>(std::move(body))};
	return mMailbox.push(peer.address(), lane, std::move(mail), timeout) == Mailbox::Push::Queued;
}


void netlink::PeerChannel::setKeepAlive(const std::string &computerName, const bool enabled)
{
	const auto peer = resolvePeer(computerName);
	if (!peer.isValid())
		return;

	mMailbox.post(enabled ? Mailbox::Command::KeepAliveOn : Mailbox::Command::KeepAliveOff, peer.address());
}


void netlink::PeerChannel::dropApplicationTraffic(const std::string &computerName)
{
	const auto peer = resolvePeer(computerName);
	if (!peer.isValid())
		return;

	mMailbox.dropApplication(peer.address());
}


bool netlink::PeerChannel::flush(const std::string &computerName, const std::chrono::milliseconds timeout)
{
	const auto peer = resolvePeer(computerName);
	if (!peer.isValid())
		return true;

	return mMailbox.flush(peer.address(), timeout);
}


void netlink::PeerChannel::start()
{
	mDelivery.start();
	mMailbox.setRunning(true);
	ThreadBase::start();
}


void netlink::PeerChannel::stop()
{
	mMailbox.setRunning(false);
	ThreadBase::stop();

	// After the I/O loop: nothing new is handed over anymore
	mDelivery.stop();
}


void netlink::PeerChannel::interruptWork()
{
	if (const auto socket = this->socket())
		socket->interrupt();
}


// ---------------------------------------------------------------------------
// I/O loop
// ---------------------------------------------------------------------------

void netlink::PeerChannel::run()
{
	try
	{
		loop();
	}
	catch (const std::exception &e)
	{
		NETLINK_LOG_ERROR("The I/O loop of the peer channel ended with an exception: {}", e.what());
		fail();
	}
	catch (...)
	{
		NETLINK_LOG_ERROR("The I/O loop of the peer channel ended with an unknown exception");
		fail();
	}
}


void netlink::PeerChannel::loop()
{
	while (ThreadBase::isRunning())
	{
		const auto socket = mInitialized.load() ? this->socket() : nullptr;

		if (!socket)
		{
			waitForEvent();
			continue;
		}

		const auto now = Clock::now();

		// How long a round takes that only waited for the next tick of the send budget
		if (std::exchange(mTickTimedOut, false))
		{
			mBudgetTicks.fetch_add(1, std::memory_order_relaxed);
			mBudgetTickTime.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - mStepStartedAt).count()), std::memory_order_relaxed);
		}

		mStepStartedAt	 = now;
		EventBatch batch = step(*socket, now);
		deliver(batch);
		waitForWork(*socket);
	}
}


void netlink::PeerChannel::fail()
{
	EventBatch batch;
	endAllSessions(batch, "the channel stopped after an internal error");

	// Nothing is acknowledged anymore: whoever waits for it gives up
	mMailbox.setRunning(false);

	deliver(batch);
}


std::optional<netlink::PeerChannel::TimePoint> netlink::PeerChannel::poll(const TimePoint now)
{
	const auto socket = mInitialized.load() ? this->socket() : nullptr;
	if (!socket)
		return std::nullopt;

	EventBatch batch = step(*socket, now);

	for (auto &[from, message] : batch.messages)
	{
		if (message.lane == Lane::Control)
			routeControl(from, message.body);
		else
			routeApplication(from, message.tag, std::move(message.body));
	}

	for (const auto &lost : batch.lostPeers)
		reportLostPeer(lost);

	return mNextWake;
}


netlink::PeerChannel::EventBatch netlink::PeerChannel::step(net::IDatagramSocket &socket, const TimePoint now)
{
	EventBatch batch;

	mSteps.fetch_add(1, std::memory_order_relaxed);

	// Links whose acknowledgements the socket did not take in the last step get their turn again
	mTouched.clear();
	mTouched.swap(mAcksHeldBack);

	mMailbox.drain(mWork);

	for (const auto &command : mWork.commands)
		apply(command, now);

	for (const auto &address : mWork.ready)
	{
		if (auto *state = linkFor(address, true))
		{
			state->unsettled = true;
			mTouched.push_back(address);
		}
	}

	updatePause();
	receivePending(socket, now);
	serviceTimers(batch, now);

	std::ranges::sort(mTouched);
	const auto duplicates = std::ranges::unique(mTouched);
	mTouched.erase(duplicates.begin(), duplicates.end());

	mScheduler.refill(now);
	mSocketBlocked = false;
	mSocketFailed  = false;

	for (const auto &address : mTouched)
	{
		auto *state = linkFor(address, false);
		if (!state)
			continue;

		collect(address, *state, batch);
		sendAcks(address, *state, socket);
		schedule(address, *state, now);
	}

	if (!mSocketBlocked)
		sendData(socket, now);

	for (const auto &address : mTouched)
	{
		auto *state = linkFor(address, false);
		if (!state)
			continue;

		state->deadline = state->link.nextDeadline();

		if (state->unsettled && !state->link.hasPendingReliable())
			state->unsettled = !mMailbox.settle(address);
	}

	// A socket that was replaced in the meantime is no failure: the next round runs on the new one
	if (mSocketFailed && this->socket().get() == &socket)
		endAllSessions(batch, "the network is not available anymore");

	mNextWake = mHeartbeat.nextDeadline();

	for (const auto &state : mLinks | std::views::values)
		mNextWake = earlier(mNextWake, state.deadline);

	// Datagrams wait for tokens or for the socket
	mWaitingForTick = mSocketBlocked || mScheduler.hasBacklog() || !mAcksHeldBack.empty();

	if (mWaitingForTick)
		mNextWake = earlier(mNextWake, std::optional{now + SendScheduler::Tick});

	return batch;
}


void netlink::PeerChannel::apply(const Mailbox::PostedCommand &command, const TimePoint now)
{
	const auto &[kind, peer] = command;

	switch (kind)
	{
	case Mailbox::Command::EraseLink:
		mLinks.erase(peer);
		mHeartbeat.unwatch(peer);
		mScheduler.remove(peer);
		break;

	case Mailbox::Command::ResetLinks: forgetLinks(); break;

	// What the mailbox dropped is not sent anymore: whoever waits for it is told
	case Mailbox::Command::DropApplication:
		if (auto *state = linkFor(peer, false))
		{
			state->unsettled = true;
			mTouched.push_back(peer);
		}
		break;

	// Only wakes the loop: every step looks at what waits for the application
	case Mailbox::Command::ResumeReceiving: break;

	case Mailbox::Command::KeepAliveOn:
		// Heartbeats need a link, also towards a peer nothing was exchanged with yet
		if (linkFor(peer, true))
			mHeartbeat.watch(peer, now);
		break;

	case Mailbox::Command::KeepAliveOff: mHeartbeat.unwatch(peer); break;

	case Mailbox::Command::Reconfigure:
	{
		std::lock_guard<std::mutex> lock(mSocketMutex);

		if (mPendingConfig.maxSendRate != mConfig.maxSendRate)
			mScheduler.setRate(mPendingConfig.maxSendRate);

		mConfig = mPendingConfig;
		mHeartbeat.setTimings(mConfig.timings);
		break;
	}
	}
}


void netlink::PeerChannel::forgetLinks()
{
	mLinks.clear();
	mHeartbeat.clear();
	mScheduler.clear();
	mAcksHeldBack.clear();
	mNextWake.reset();
}


void netlink::PeerChannel::endAllSessions(EventBatch &batch, const char *reason)
{
	for (const auto &address : mLinks | std::views::keys)
		batch.lostPeers.push_back({.address = address, .reason = reason});

	forgetLinks();
	mMailbox.reset();
}


void netlink::PeerChannel::receivePending(net::IDatagramSocket &socket, const TimePoint now)
{
	// Everything that is waiting is taken in before anything is answered: one send pass and one hand-over for all of it
	for (size_t received = 0; received < MaxDatagramsPerPass; ++received)
	{
		const auto datagram = socket.receiveFrom(mReceiveBuffer, std::chrono::microseconds::zero());

		// Larger than any datagram of a channel: the operating system dropped it, whatever waits behind it is still read
		if (!datagram && datagram.error() == net::SocketError::MessageTooLarge)
		{
			NETLINK_LOG_DEBUG("Dropping a datagram that exceeds the receive buffer");
			continue;
		}

		if (!datagram)
			break;

		handleDatagram(datagram->from, std::span<const uint8_t>(mReceiveBuffer.data(), datagram->size), now);
	}
}


void netlink::PeerChannel::handleDatagram(const net::SocketAddress &from, const std::span<const uint8_t> bytes, const TimePoint now)
{
	const auto packet = channel::decodePacket(bytes);

	if (!packet)
	{
		NETLINK_LOG_DEBUG("Dropping malformed datagram from {} ({} bytes)", from.toString(), bytes.size());
		return;
	}

	auto *state = linkFor(from, true);
	if (!state)
		return;

	state->link.onPacket(*packet, now);
	mHeartbeat.onReceived(from, now);
	mTouched.push_back(from);
}


void netlink::PeerChannel::serviceTimers(EventBatch &batch, const TimePoint now)
{
	for (auto &[address, state] : mLinks)
	{
		if (state.deadline && *state.deadline <= now)
		{
			state.link.onTimer(now);
			mTouched.push_back(address);
		}
	}

	if (const auto due = mHeartbeat.nextDeadline(); !due || now < *due)
		return;

	auto [pingsDue, silentPeers] = mHeartbeat.tick(now);

	for (const auto &address : pingsDue)
	{
		if (auto *state = linkFor(address, false))
		{
			state->link.sendPing();
			mTouched.push_back(address);
		}
	}

	for (const auto &address : silentPeers)
		batch.lostPeers.push_back({.address = address, .reason = "no traffic from the peer anymore"});
}


void netlink::PeerChannel::updatePause()
{
	// Too much waits for the application: every sender is asked to hold back, and told when there is room again
	const size_t waiting = mDeliveryBacklog->bytes.load();

	if (mPaused ? waiting >= channel::BacklogResumeBytes : waiting <= channel::BacklogPauseBytes)
		return;

	mPaused = !mPaused;
	mDeliveryBacklog->paused.store(mPaused);

	for (auto &[address, state] : mLinks)
	{
		state.link.setPaused(mPaused);
		mTouched.push_back(address);
	}
}


void netlink::PeerChannel::collect(const net::SocketAddress &address, LinkState &state, EventBatch &batch)
{
	auto &link = state.link;

	for (const auto &event : link.takeEvents())
	{
		mHeartbeat.unwatch(address);

		const char *reason = event == channel::LinkEvent::Failed ? "the peer stopped acknowledging messages" : "the peer restarted";
		batch.lostPeers.push_back({.address = address, .reason = reason});
	}

	for (auto &message : link.takeDelivered())
		batch.messages.push_back({.from = address, .message = std::move(message)});
}


void netlink::PeerChannel::sendAcks(const net::SocketAddress &address, LinkState &state, net::IDatagramSocket &socket)
{
	// Not held back by the budget, but counted against it
	while (const auto *ack = state.link.peekAck())
	{
		if (mSocketBlocked || transmit(socket, address, *ack) == Transmit::Refused)
		{
			mAcksHeldBack.push_back(address);
			return;
		}

		state.link.commitAck();
		mScheduler.spend();
	}
}


void netlink::PeerChannel::schedule(const net::SocketAddress &address, LinkState &state, const TimePoint now)
{
	MailboxSource source(mMailbox, address);

	for (const Lane lane : SendScheduler::Order)
	{
		if ((state.scheduled & bitOf(lane)) == 0 && state.link.peek(lane, now, source))
		{
			mScheduler.add(lane, address);
			state.scheduled |= bitOf(lane);
		}
	}
}


void netlink::PeerChannel::sendData(net::IDatagramSocket &socket, const TimePoint now)
{
	mScheduler.run(
		[&](const Lane lane, const net::SocketAddress &address)
		{
			MailboxSource source(mMailbox, address);

			auto	   *state	 = linkFor(address, false);
			const auto	 *datagram = state ? state->link.peek(lane, now, source) : nullptr;

			if (!datagram)
			{
				if (state)
					state->scheduled &= static_cast<uint8_t>(~bitOf(lane));

				return SendScheduler::Result::Empty;
			}

			const Transmit outcome = transmit(socket, address, *datagram);
			if (outcome == Transmit::Refused)
				return SendScheduler::Result::Blocked;

			state->link.commit(lane, now);
			state->deadline = state->link.nextDeadline();

			return outcome == Transmit::Sent ? SendScheduler::Result::Sent : SendScheduler::Result::Lost;
		});
}


netlink::PeerChannel::Transmit netlink::PeerChannel::transmit(net::IDatagramSocket &socket, const net::SocketAddress &address, const channel::OutgoingDatagram &datagram)
{
	const auto sent = socket.sendParts(address, datagram.header(), datagram.body());

	if (sent)
	{
		mDatagramsSent.fetch_add(1, std::memory_order_relaxed);
		return Transmit::Sent;
	}

	switch (sent.error())
	{
	case net::SocketError::WouldBlock: mSocketBlocked = true; return Transmit::Refused;

	case net::SocketError::NetworkDown:
	case net::SocketError::AddressNotAvailable:
	case net::SocketError::Closed:
	case net::SocketError::NotInitialized:
		NETLINK_LOG_WARNING("The channel socket cannot send anymore: {}", net::toString(sent.error()));
		mSocketBlocked = true;
		mSocketFailed  = true;
		return Transmit::Refused;

	default:
		// The destination cannot be reached right now: no different from a datagram that got lost on the way
		NETLINK_LOG_DEBUG("Sending to {} failed: {}", address.toString(), net::toString(sent.error()));
		return Transmit::Lost;
	}
}


void netlink::PeerChannel::waitForWork(net::IDatagramSocket &socket)
{
	std::chrono::microseconds timeout = MaxWait;

	if (mNextWake)
	{
		const auto now = Clock::now();

		if (*mNextWake <= now)
		{
			// Still due after the step: retried shortly, without spinning
			timeout = std::chrono::milliseconds{1};
			mOverdueWaits.fetch_add(1, std::memory_order_relaxed);
		}
		else if (*mNextWake - now < MaxWait)
			timeout = std::chrono::ceil<std::chrono::microseconds>(*mNextWake - now);
	}

	const auto ready = socket.waitReadable(timeout);
	mTickTimedOut	 = mWaitingForTick && !ready && ready.error() == net::SocketError::Timeout;

	// A broken socket fails right away: do not spin on it
	if (!ready && ready.error() != net::SocketError::Timeout && ready.error() != net::SocketError::Cancelled)
		waitForEvent(static_cast<unsigned long>(internal::SocketPollInterval.count()));
}


netlink::PeerChannel::LinkState *netlink::PeerChannel::linkFor(const net::SocketAddress &address, const bool create)
{
	if (const auto it = mLinks.find(address); it != mLinks.end())
		return &it->second;

	if (!create)
		return nullptr;

	if (mLinks.size() >= MaxLinks)
	{
		NETLINK_LOG_WARNING("Too many remotes, ignoring {}", address.toString());
		return nullptr;
	}

	LinkState &state = mLinks.try_emplace(address, mConfig.timings, &mAssemblyBudget).first->second;
	state.link.setPaused(mPaused);
	return &state;
}


// ---------------------------------------------------------------------------
// Handing over to the delivery threads
// ---------------------------------------------------------------------------

void netlink::PeerChannel::deliver(EventBatch &batch)
{
	if (batch.messages.empty() && batch.lostPeers.empty())
		return;

	// What waits for the application is accounted for until its task is gone
	const auto shareOf = [this](const std::vector<InboundMessage> &messages)
	{
		size_t bytes = 0;
		for (const auto &[from, message] : messages)
			bytes += message.lane == Lane::Reliable || message.lane == Lane::Bulk ? message.body.size() : 0;

		return std::make_shared<BacklogShare>(mDeliveryBacklog, bytes);
	};

	std::vector<InboundMessage> application;

	// Application messages may have a thread of their own, so a slow application cannot hold up control signals
	if (mApplicationQueue)
	{
		const auto moved = std::ranges::stable_partition(batch.messages, [](const InboundMessage &inbound) { return inbound.message.lane == Lane::Control; });

		application.assign(std::make_move_iterator(moved.begin()), std::make_move_iterator(moved.end()));
		batch.messages.erase(moved.begin(), moved.end());
	}

	if (!application.empty())
	{
		mApplicationQueue->post(
			[this, share = shareOf(application), messages = std::move(application)]() mutable
			{
				for (auto &[from, message] : messages)
					routeApplication(from, message.tag, std::move(message.body));
			});
	}

	if (batch.messages.empty() && batch.lostPeers.empty())
		return;

	mDelivery.post(
		[this, share = shareOf(batch.messages), messages = std::move(batch.messages), lostPeers = std::move(batch.lostPeers)]() mutable
		{
			for (auto &[from, message] : messages)
			{
				if (message.lane == Lane::Control)
					routeControl(from, message.body);
				else
					routeApplication(from, message.tag, std::move(message.body));
			}

			for (const auto &lost : lostPeers)
				reportLostPeer(lost);
		});
}


void netlink::PeerChannel::reportLostPeer(const LostPeer &lost) const
{
	const std::string name = nameOf(lost.address);
	NETLINK_LOG_WARNING("Lost peer {} ({}): {}", name.empty() ? "<unknown>" : name, lost.address.toString(), lost.reason);

	if (!name.empty() && mOnPeerLost)
		mOnPeerLost(name, lost.reason);
}


void netlink::PeerChannel::routeApplication(const net::SocketAddress &from, const uint32_t type, std::vector<uint8_t> body) const
{
	const std::string name = nameOf(from);

	if (name.empty())
	{
		NETLINK_LOG_WARNING("Dropping application message from unknown remote {}", from.toString());
		return;
	}

	// The payload stays in the buffer it was reassembled in
	if (mMessageCallback)
		mMessageCallback(name, type, std::move(body));
}


void netlink::PeerChannel::routeControl(const net::SocketAddress &from, std::span<const uint8_t> body) const
{
	SignalPacket packet;

	try
	{
		packet = json::parse(body.begin(), body.end()).get<SignalPacket>();
	}
	catch (const std::exception &e)
	{
		NETLINK_LOG_ERROR("Error parsing signal packet from {}: {}", from.toString(), e.what());
		return;
	}

	const std::string &sender = packet.senderName;

	switch (packet.signalType)
	{
	case SignalType::ConnectRequest:
	{
		if (mConnectionCallbacks.onConnectRequested)
			mConnectionCallbacks.onConnectRequested(sender);
		break;
	}

	case SignalType::ConnectAnswer:
	{
		const auto &[accepted, reason] = std::get<PayloadConnectAnswer>(packet.payload);
		if (mConnectionCallbacks.onConnectRequestAnswered)
			mConnectionCallbacks.onConnectRequestAnswered(sender, accepted, reason);
		break;
	}

	case SignalType::Disconnect:
		if (mConnectionCallbacks.onDisconnectReceived)
			mConnectionCallbacks.onDisconnectReceived(sender);
		break;

	case SignalType::ReadyFlag:
		if (mConnectionCallbacks.onReadyFlagReceived)
			mConnectionCallbacks.onReadyFlagReceived(sender);
		break;

	case SignalType::ValidationRequest:
	{
		const auto &[request] = std::get<PayloadValidationRequest>(packet.payload);
		if (request != static_cast<uint8_t>(RemoteRequest::Secret) && request != static_cast<uint8_t>(RemoteRequest::Version))
		{
			NETLINK_LOG_WARNING("Ignoring unknown validation request {} from {}", static_cast<int>(request), sender);
			break;
		}

		if (mValidationCallbacks.onValidationRequestReceived)
			mValidationCallbacks.onValidationRequestReceived(sender, static_cast<RemoteRequest>(request));
		break;
	}

	case SignalType::SecretResponse:
	{
		const auto &[secret] = std::get<PayloadSecretResponse>(packet.payload);
		if (mValidationCallbacks.onSecretResponseReceived)
			mValidationCallbacks.onSecretResponseReceived(sender, secret);
		break;
	}

	case SignalType::VersionResponse:
	{
		const auto &[version] = std::get<PayloadVersionResponse>(packet.payload);
		if (mValidationCallbacks.onVersionResponseReceived)
			mValidationCallbacks.onVersionResponseReceived(sender, version);
		break;
	}

	case SignalType::ValidationHandshake:
		if (mValidationCallbacks.onValidationHandshakeReceived)
			mValidationCallbacks.onValidationHandshakeReceived(sender);
		break;

	default: NETLINK_LOG_WARNING("Unknown signal type received: {}", static_cast<int>(packet.signalType)); break;
	}
}
