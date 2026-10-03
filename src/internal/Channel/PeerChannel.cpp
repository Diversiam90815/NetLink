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

#include "NetLinkConstants.h"
#include "NetLinkLog.h"
#include "Socket/UdpSocket.h"

using json = nlohmann::json;
using netlink::channel::Mailbox;


namespace
{

// The longest the I/O thread waits without anything to do
constexpr auto	MaxWait = std::chrono::hours{1};

Mailbox::Limits limitsOf(const netlink::PeerChannelConfig &config)
{
	const auto &reliability = config.reliability;

	return {.controlCapacity	 = netlink::channel::ReliableLink::ControlQueueCapacity,
			.applicationCapacity = reliability.sendQueueCapacity,
			.applicationOverflow = reliability.sendQueueOverflow,
			.unreliableCapacity	 = reliability.unreliableQueueCapacity,
			.maxMessageSize		 = reliability.maxMessageSize,
			.maxUnreliableBody	 = netlink::channel::ReliableLink(reliability).maxUnreliableBody()};
}

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
	  mMailbox([this] { wakeIoThread(); }), mConfig(config), mHeartbeat(config.heartbeat), mReceiveBuffer(internal::PackageBufferSize)
{
	mMailbox.setLimits(limitsOf(config));
}


netlink::PeerChannel::~PeerChannel()
{
	deinit();
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
	mLinks.clear();
	mHeartbeat.clear();
	mNextWake.reset();
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

	mMailbox.setLimits(limitsOf(config));
	mMailbox.post(Mailbox::Command::Reconfigure);
}


void netlink::PeerChannel::setLocalIPv4(const net::IPv4Address &localIPv4)
{
	if (localIPv4.isUnspecified())
		return;

	auto socket = mSocketFactory({.ip = localIPv4, .port = 0}, {.receiveBufferSize = internal::ChannelReceiveBufferSize});

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
	const bool		  queued  = push(peer, Mailbox::Lane::Control, 0, std::vector<uint8_t>(encoded.begin(), encoded.end()));

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
	if (mode == DeliveryMode::ReliableOrdered)
		return push(peer, Mailbox::Lane::Application, type, std::vector<uint8_t>(data.begin(), data.end()), timeout);

	return push(peer, Mailbox::Lane::Unreliable, type, std::vector<uint8_t>(data.begin(), data.end()));
}


bool netlink::PeerChannel::push(const PeerEndpoint &peer, const Mailbox::Lane lane, const uint32_t tag, std::vector<uint8_t> body, const std::chrono::milliseconds timeout)
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

	return mMailbox.push(peer.address(), lane, {.tag = tag, .body = std::move(body)}, timeout) == Mailbox::Push::Queued;
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

		EventBatch batch = step(*socket, Clock::now());
		deliver(batch);
		waitForWork(*socket);
	}
}


void netlink::PeerChannel::fail()
{
	EventBatch batch;

	for (const auto &address : mLinks | std::views::keys)
		batch.lostPeers.push_back({.address = address, .reason = "the channel stopped after an internal error"});

	mLinks.clear();
	mHeartbeat.clear();
	mNextWake.reset();

	// Nothing is acknowledged anymore: whoever waits for it gives up
	mMailbox.setRunning(false);
	mMailbox.reset();

	deliver(batch);
}


netlink::PeerChannel::EventBatch netlink::PeerChannel::step(net::IDatagramSocket &socket, const TimePoint now)
{
	EventBatch batch;

	mSteps.fetch_add(1, std::memory_order_relaxed);
	mTouched.clear();

	mMailbox.drain(mWork);

	for (const auto &command : mWork.commands)
		apply(command, now);

	for (const auto &address : mWork.ready)
	{
		if (auto *state = linkFor(address, true))
		{
			state->backlog	 = true;
			state->unsettled = true;
			mTouched.push_back(address);
		}
	}

	receivePending(socket, now);
	serviceTimers(batch, now);

	std::ranges::sort(mTouched);
	const auto duplicates = std::ranges::unique(mTouched);
	mTouched.erase(duplicates.begin(), duplicates.end());

	for (const auto &address : mTouched)
	{
		auto *state = linkFor(address, false);
		if (!state)
			continue;

		if (state->backlog)
			feed(address, *state);

		collect(address, *state, socket, batch, now);

		if (state->unsettled && !state->backlog && !state->link.hasPendingReliable())
			state->unsettled = !mMailbox.settle(address);
	}

	mNextWake = earlier(mNextWake, mHeartbeat.nextDeadline());
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
		break;

	case Mailbox::Command::ResetLinks:
		mLinks.clear();
		mHeartbeat.clear();
		mNextWake.reset();
		break;

	case Mailbox::Command::DropApplication:
		if (auto *state = linkFor(peer, false))
		{
			state->link.dropQueuedApplicationMessages();
			state->unsettled = true;
			mTouched.push_back(peer);
		}
		break;

	case Mailbox::Command::KeepAliveOn:
		// Heartbeats need a link, also towards a peer nothing was exchanged with yet
		if (linkFor(peer, true))
			mHeartbeat.watch(peer, now);
		break;

	case Mailbox::Command::KeepAliveOff: mHeartbeat.unwatch(peer); break;

	case Mailbox::Command::Reconfigure:
	{
		std::lock_guard<std::mutex> lock(mSocketMutex);
		mConfig = mPendingConfig;
		mHeartbeat.setConfig(mConfig.heartbeat);
		break;
	}
	}
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
	if (!mNextWake || now < *mNextWake)
		return;

	// Rebuilt from what the links and the heartbeats report in this step
	mNextWake.reset();

	for (auto &[address, state] : mLinks)
	{
		state.link.onTimer(now);
		mTouched.push_back(address);
	}

	auto [heartbeatsDue, silentPeers] = mHeartbeat.tick(now);

	for (const auto &address : heartbeatsDue)
	{
		if (auto *state = linkFor(address, false))
			state->link.sendHeartbeat();
	}

	for (const auto &address : silentPeers)
		batch.lostPeers.push_back({.address = address, .reason = "no traffic from the peer anymore"});
}


void netlink::PeerChannel::feed(const net::SocketAddress &address, LinkState &state)
{
	auto &link	  = state.link;

	state.backlog = mMailbox.feed(address,
								  [&link](const Mailbox::Lane lane, Mailbox::Mail &mail)
								  {
									  if (lane == Mailbox::Lane::Unreliable)
									  {
										  link.sendUnreliable(channel::ChannelId::Application, mail.tag, mail.body);
										  return true;
									  }

									  const auto channelId = lane == Mailbox::Lane::Control ? channel::ChannelId::Control : channel::ChannelId::Application;
									  if (!link.hasRoomFor(channelId))
										  return false;

									  link.queueReliable(channelId, mail.tag, std::move(mail.body));
									  return true;
								  });
}


void netlink::PeerChannel::collect(const net::SocketAddress &address, LinkState &state, net::IDatagramSocket &socket, EventBatch &batch, const TimePoint now)
{
	auto &link = state.link;

	for (const auto &event : link.takeEvents())
	{
		mHeartbeat.unwatch(address);

		const char *reason = event == channel::LinkEvent::Failed ? "the peer stopped acknowledging messages" : "the peer restarted";
		batch.lostPeers.push_back({.address = address, .reason = reason});
	}

	// The acknowledgements of this pass tell the remote whether the application keeps up with what it sends
	link.setApplicationReceiving(mDeliveryBacklog->load() < mConfig.deliveryBacklogLimit);

	// One send pass: acknowledgements, and as much data as the link's windows allow
	const auto datagrams = link.takeOutgoing(now);

	if (!datagrams.empty())
		mHeartbeat.onSent(address, now);

	for (const auto &datagram : datagrams)
	{
		// A lost datagram is recovered by retransmission, a failed send is no different
		if (auto sent = socket.sendParts(address, datagram.header(), datagram.body()); !sent)
			NETLINK_LOG_DEBUG("Sending to {} failed: {}", address.toString(), net::toString(sent.error()));
	}

	mNextWake = earlier(mNextWake, link.nextDeadline());

	for (auto &message : link.takeDelivered())
		batch.messages.push_back({.from = address, .message = std::move(message)});
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

	return &mLinks.try_emplace(address, mConfig.reliability).first->second;
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
			bytes += message.channel == channel::ChannelId::Application ? message.body.size() : 0;

		return std::make_shared<BacklogShare>(mDeliveryBacklog, bytes);
	};

	std::vector<InboundMessage> application;

	// Application messages may have a thread of their own, so a slow application cannot hold up control signals
	if (mApplicationQueue)
	{
		const auto moved = std::ranges::stable_partition(batch.messages, [](const InboundMessage &inbound) { return inbound.message.channel == channel::ChannelId::Control; });

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
				if (message.channel == channel::ChannelId::Control)
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
