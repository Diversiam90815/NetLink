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


netlink::PeerChannel::PeerChannel(net::DatagramSocketFactory socketFactory, const PeerChannelConfig &config, TaskQueue *applicationQueue)
	: mSocketFactory(socketFactory ? std::move(socketFactory) : net::UdpSocket::factory()), mReceiveBuffer(internal::PackageBufferSize), mApplicationQueue(applicationQueue),
	  mConfig(config), mHeartbeat(config.heartbeat)
{
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

	resetLinks();

	mBoundPort.store(0);
	mInitialized.store(false);
}


void netlink::PeerChannel::setConfig(const PeerChannelConfig &config)
{
	std::lock_guard<std::mutex> lock(mLinksMutex);
	mConfig = config;
	mHeartbeat.setConfig(config.heartbeat);
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
	{
		std::lock_guard<std::mutex> lock(mLinksMutex);
		mLinks.clear();
		mHeartbeat.clear();
		mEarliestDeadline.reset();
	}
	mLinksChanged.notify_all();
}


// ---------------------------------------------------------------------------
// Peer registry
// ---------------------------------------------------------------------------

void netlink::PeerChannel::registerPeer(const std::string &displayName, const net::IPv4Address &ipv4, const int channelPort)
{
	PeerEndpoint previous;

	{
		std::lock_guard<std::mutex> lock(mPeerRegistryMutex);

		if (const auto it = mPeerRegistry.find(displayName); it != mPeerRegistry.end())
		{
			previous = it->second;
			forgetAddress(previous.address(), displayName);
		}

		const PeerEndpoint endpoint{.IPv4 = ipv4, .channelPort = channelPort};
		mPeerRegistry[displayName]			= endpoint;
		mNameByAddress[endpoint.address()] = displayName;
	}

	NETLINK_LOG_DEBUG("Registered peer {} -> {}:{}", displayName, ipv4.toString(), channelPort);

	// The peer moved to another socket (restart, adapter switch): its old stream is gone
	if (previous.isValid() && previous.address() != PeerEndpoint{.IPv4 = ipv4, .channelPort = channelPort}.address())
	{
		{
			std::lock_guard<std::mutex> lock(mLinksMutex);
			mLinks.erase(previous.address());
			mHeartbeat.unwatch(previous.address());
		}
		mLinksChanged.notify_all();
	}
}


void netlink::PeerChannel::unregisterPeer(const std::string &displayName)
{
	PeerEndpoint removed;

	{
		std::lock_guard<std::mutex> lock(mPeerRegistryMutex);

		if (const auto it = mPeerRegistry.find(displayName); it != mPeerRegistry.end())
		{
			removed = it->second;
			mPeerRegistry.erase(it);
			forgetAddress(removed.address(), displayName);
		}
	}

	if (removed.isValid())
	{
		{
			std::lock_guard<std::mutex> lock(mLinksMutex);
			mLinks.erase(removed.address());
			mHeartbeat.unwatch(removed.address());
		}
		mLinksChanged.notify_all();
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
	const bool		  queued  = queueReliable(peer, channel::ChannelId::Control, 0, std::vector<uint8_t>(encoded.begin(), encoded.end()));

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
		return queueReliable(peer, channel::ChannelId::Application, type, std::vector<uint8_t>(data.begin(), data.end()), timeout);

	if (!mInitialized.load() || !socket())
		return false;

	Batch batch;
	bool  sent = false;

	{
		std::lock_guard<std::mutex> lock(mLinksMutex);

		auto					   *link = linkFor(peer.address(), true);
		if (!link)
			return false;

		sent = link->sendUnreliable(channel::ChannelId::Application, type, data);
		collectOutgoing(peer.address(), *link, batch, Clock::now());
	}

	execute(batch);
	return sent;
}


bool netlink::PeerChannel::queueReliable(const PeerEndpoint &peer, const channel::ChannelId channelId, const uint32_t tag, std::vector<uint8_t> body,
										 const std::chrono::milliseconds timeout)
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

	Batch batch;
	bool  queued = false;

	{
		std::unique_lock<std::mutex> lock(mLinksMutex);

		auto						*link = linkFor(peer.address(), true);
		if (!link)
			return false;

		// Backpressure: acknowledgements make room, and the I/O loop signals every one of them
		if (timeout > std::chrono::milliseconds::zero() && !link->hasRoomFor(channelId))
		{
			mLinksChanged.wait_for(lock, timeout,
								   [&]
								   {
									   link = linkFor(peer.address(), true); // the link may have been replaced while waiting
									   return !link || link->hasRoomFor(channelId) || !isRunning();
								   });

			if (!link)
				return false;
		}

		queued = link->queueReliable(channelId, tag, std::move(body)) != channel::PushResult::Rejected;
		collectOutgoing(peer.address(), *link, batch, Clock::now());
	}

	execute(batch);
	return queued;
}


void netlink::PeerChannel::setKeepAlive(const std::string &computerName, const bool enabled)
{
	const auto peer = resolvePeer(computerName);
	if (!peer.isValid())
		return;

	bool wake = false;

	{
		std::lock_guard<std::mutex> lock(mLinksMutex);

		if (enabled)
		{
			// Heartbeats need a link, also towards a peer nothing was exchanged with yet
			linkFor(peer.address(), true);
			mHeartbeat.watch(peer.address(), Clock::now());
			wake = noteDeadline(mHeartbeat.nextDeadline());
		}
		else
			mHeartbeat.unwatch(peer.address());
	}

	if (wake)
	{
		if (const auto socket = this->socket())
			socket->interrupt();
	}
}


void netlink::PeerChannel::dropApplicationTraffic(const std::string &computerName)
{
	const auto peer = resolvePeer(computerName);
	if (!peer.isValid())
		return;

	std::lock_guard<std::mutex> lock(mLinksMutex);

	if (auto *link = linkFor(peer.address(), false))
		link->dropQueuedApplicationMessages();
}


bool netlink::PeerChannel::flush(const std::string &computerName, const std::chrono::milliseconds timeout)
{
	const auto peer = resolvePeer(computerName);
	if (!peer.isValid())
		return true;

	std::unique_lock<std::mutex> lock(mLinksMutex);

	const auto					 settled = [&]
	{
		const auto *link = linkFor(peer.address(), false);
		return !link || !link->hasPendingReliable();
	};

	// Acknowledgements are only processed by the running I/O loop, which signals every change
	mLinksChanged.wait_until(lock, Clock::now() + timeout, [&] { return settled() || !isRunning(); });
	return settled();
}


void netlink::PeerChannel::start()
{
	mDelivery.start();
	ThreadBase::start();
}


void netlink::PeerChannel::stop()
{
	ThreadBase::stop();

	// Taking the lock orders this wake-up after a waiter's last check, so it cannot be missed
	{
		std::lock_guard<std::mutex> lock(mLinksMutex);
	}
	mLinksChanged.notify_all();

	// After the I/O loop: nothing new is handed over anymore
	mDelivery.stop();
}


void netlink::PeerChannel::interruptWork()
{
	if (const auto socket = this->socket())
		socket->interrupt();
}


// ---------------------------------------------------------------------------
// Link bookkeeping (caller holds mLinksMutex)
// ---------------------------------------------------------------------------

netlink::channel::ReliableLink *netlink::PeerChannel::linkFor(const net::SocketAddress &address, const bool create)
{
	if (const auto it = mLinks.find(address); it != mLinks.end())
		return it->second.get();

	if (!create)
		return nullptr;

	if (mLinks.size() >= MaxLinks)
	{
		NETLINK_LOG_WARNING("Too many remotes, ignoring {}", address.toString());
		return nullptr;
	}

	auto link = std::make_unique<channel::ReliableLink>(mConfig.reliability);
	return mLinks.emplace(address, std::move(link)).first->second.get();
}


void netlink::PeerChannel::collect(const net::SocketAddress &address, channel::ReliableLink &link, Batch &batch, const TimePoint now)
{
	for (const auto &event : link.takeEvents())
	{
		mHeartbeat.unwatch(address);

		const char *reason = event == channel::LinkEvent::Failed ? "the peer stopped acknowledging messages" : "the peer restarted";
		batch.lostPeers.push_back({.address = address, .reason = reason});
	}

	collectOutgoing(address, link, batch, now);

	for (auto &message : link.takeDelivered())
		batch.messages.push_back({.from = address, .message = std::move(message)});
}


void netlink::PeerChannel::collectOutgoing(const net::SocketAddress &address, channel::ReliableLink &link, Batch &batch, const TimePoint now)
{
	// The acknowledgements of this pass tell the remote whether the application keeps up with what it sends
	link.setApplicationReceiving(mDeliveryBacklog->load() < mConfig.deliveryBacklogLimit);

	// One send pass: acknowledgements, and as much data as the link's windows allow
	auto datagrams = link.takeOutgoing(now);

	if (!datagrams.empty())
		mHeartbeat.onSent(address, now);

	for (auto &datagram : datagrams)
		batch.datagrams.push_back({.to = address, .datagram = std::move(datagram)});

	if (noteDeadline(link.nextDeadline()))
		batch.wakeIoThread = true;
}


// ---------------------------------------------------------------------------
// I/O loop
// ---------------------------------------------------------------------------

void netlink::PeerChannel::run()
{
	while (ThreadBase::isRunning())
	{
		const auto socket = mInitialized.load() ? this->socket() : nullptr;

		if (!socket)
		{
			waitForEvent(static_cast<unsigned long>(internal::SocketPollInterval.count()));
			continue;
		}

		receivePending(*socket);
		serviceTimers();
		waitForWork(*socket);
	}
}


bool netlink::PeerChannel::noteDeadline(const std::optional<TimePoint> due)
{
	if (!due || (mEarliestDeadline && *mEarliestDeadline <= *due))
		return false;

	mEarliestDeadline = due;
	return mWaitingUntil && *due < *mWaitingUntil;
}


void netlink::PeerChannel::receivePending(net::IDatagramSocket &socket)
{
	mTouched.clear();

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

		handleDatagram(datagram->from, std::span<const uint8_t>(mReceiveBuffer.data(), datagram->size));
	}

	if (mTouched.empty())
		return;

	std::ranges::sort(mTouched);
	const auto duplicates = std::ranges::unique(mTouched);
	mTouched.erase(duplicates.begin(), duplicates.end());

	Batch batch;

	{
		std::lock_guard<std::mutex> lock(mLinksMutex);

		const auto					now = Clock::now();

		for (const auto &address : mTouched)
		{
			if (auto *link = linkFor(address, false))
				collect(address, *link, batch, now);
		}
	}

	mLinksChanged.notify_all(); // acknowledgements may have completed a flush() or made room in a send queue
	execute(batch);
}


void netlink::PeerChannel::handleDatagram(const net::SocketAddress &from, const std::span<const uint8_t> bytes)
{
	const auto packet = channel::decodePacket(bytes);

	if (!packet)
	{
		NETLINK_LOG_DEBUG("Dropping malformed datagram from {} ({} bytes)", from.toString(), bytes.size());
		return;
	}

	std::lock_guard<std::mutex> lock(mLinksMutex);

	auto					   *link = linkFor(from, true);
	if (!link)
		return;

	const auto now = Clock::now();
	link->onPacket(*packet, now);
	mHeartbeat.onReceived(from, now);
	mTouched.push_back(from);
}


void netlink::PeerChannel::serviceTimers()
{
	Batch batch;

	{
		std::lock_guard<std::mutex> lock(mLinksMutex);

		const auto					now = Clock::now();

		if (!mEarliestDeadline || now < *mEarliestDeadline)
			return;

		// Rebuilt from what the links and the heartbeats report below
		mEarliestDeadline.reset();

		for (auto &[address, link] : mLinks)
		{
			link->onTimer(now);
			collect(address, *link, batch, now);
		}

		auto [heartbeatsDue, silentPeers] = mHeartbeat.tick(now);

		for (const auto &address : heartbeatsDue)
		{
			if (auto *link = linkFor(address, false))
			{
				link->sendHeartbeat();
				collect(address, *link, batch, now);
			}
		}

		for (const auto &address : silentPeers)
			batch.lostPeers.push_back({.address = address, .reason = "no traffic from the peer anymore"});

		noteDeadline(mHeartbeat.nextDeadline());
	}

	batch.wakeIoThread = false; // this is the I/O thread
	mLinksChanged.notify_all(); // a failed link was reset: nothing is pending on it anymore
	execute(batch);
}


void netlink::PeerChannel::waitForWork(net::IDatagramSocket &socket)
{
	std::chrono::microseconds timeout{0};

	{
		std::lock_guard<std::mutex> lock(mLinksMutex);

		// A timer that is still due after serviceTimers() is retried shortly, without spinning
		const auto now	 = Clock::now();
		const auto until = std::clamp(mEarliestDeadline.value_or(TimePoint::max()), now + std::chrono::milliseconds{1}, now + internal::SocketPollInterval);

		mWaitingUntil = until;
		timeout		  = std::chrono::ceil<std::chrono::microseconds>(until - now);
	}

	const auto ready = socket.waitReadable(timeout);

	{
		std::lock_guard<std::mutex> lock(mLinksMutex);
		mWaitingUntil.reset();
	}

	// A broken socket fails right away: do not spin on it
	if (!ready && ready.error() != net::SocketError::Timeout && ready.error() != net::SocketError::Cancelled)
		waitForEvent(static_cast<unsigned long>(internal::SocketPollInterval.count()));
}


// ---------------------------------------------------------------------------
// Carrying out collected work (mLinksMutex not held)
// ---------------------------------------------------------------------------

void netlink::PeerChannel::execute(Batch &batch)
{
	if (!batch.datagrams.empty() || batch.wakeIoThread)
	{
		if (const auto socket = this->socket())
		{
			for (const auto &[to, datagram] : batch.datagrams)
			{
				// A lost datagram is recovered by retransmission, a failed send is no different
				if (auto sent = socket->sendParts(to, datagram.header(), datagram.body()); !sent)
					NETLINK_LOG_DEBUG("Sending to {} failed: {}", to.toString(), net::toString(sent.error()));
			}

			if (batch.wakeIoThread)
				socket->interrupt();
		}
	}

	if (!batch.messages.empty() || !batch.lostPeers.empty())
		deliver(batch);
}


void netlink::PeerChannel::deliver(Batch &batch)
{
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
