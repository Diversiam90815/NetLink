/*
  ==============================================================================
	Module:         PeerChannel
	Description:    The dedicated UDP socket all peer-to-peer traffic runs on
  ==============================================================================
*/

#include "PeerChannel.h"

#include <algorithm>
#include <thread>

#include "NetLinkConstants.h"
#include "NetLinkLog.h"
#include "Protocol/ByteOrder.h"
#include "Socket/UdpSocket.h"

using json = nlohmann::json;


netlink::PeerChannel::PeerChannel(net::DatagramSocketFactory socketFactory, const PeerChannelConfig &config)
	: mSocketFactory(socketFactory ? std::move(socketFactory) : net::UdpSocket::factory()), mReceiveBuffer(internal::PackageBufferSize), mConfig(config),
	  mFragmentation(config.reliability.maxMessageSize), mHeartbeat(config.heartbeat)
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
	ThreadBase::stop();

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
	mConfig		   = config;
	mFragmentation = channel::FragmentationService(config.reliability.maxMessageSize);
	mHeartbeat.setConfig(config.heartbeat);
}


void netlink::PeerChannel::setLocalIPv4(const net::IPv4Address &localIPv4)
{
	if (localIPv4.isUnspecified())
		return;

	auto socket = mSocketFactory({.ip = localIPv4, .port = 0}, {});

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
	std::lock_guard<std::mutex> lock(mLinksMutex);
	mLinks.clear();
	mFragmentation.clear();
	mHeartbeat.clear();
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
			previous = it->second;

		mPeerRegistry[displayName] = {.IPv4 = ipv4, .channelPort = channelPort};
	}

	NETLINK_LOG_DEBUG("Registered peer {} -> {}:{}", displayName, ipv4.toString(), channelPort);

	// The peer moved to another socket (restart, adapter switch): its old stream is gone
	if (previous.isValid() && previous.address() != PeerEndpoint{.IPv4 = ipv4, .channelPort = channelPort}.address())
	{
		std::lock_guard<std::mutex> lock(mLinksMutex);
		mLinks.erase(previous.address());
		mFragmentation.reset(previous.address());
		mHeartbeat.unwatch(previous.address());
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
		}
	}

	if (removed.isValid())
	{
		std::lock_guard<std::mutex> lock(mLinksMutex);
		mLinks.erase(removed.address());
		mFragmentation.reset(removed.address());
		mHeartbeat.unwatch(removed.address());
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

	for (const auto &[name, endpoint] : mPeerRegistry)
	{
		if (endpoint.address() == address)
			return name;
	}

	return {};
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
	const bool		  queued  = queueReliable(peer, channel::ChannelId::Control, std::vector<uint8_t>(encoded.begin(), encoded.end()));

	if (queued)
		NETLINK_LOG_DEBUG("Signal queued for {} (type={})", computerName, static_cast<int>(type));

	return queued;
}


bool netlink::PeerChannel::sendMessage(const std::string &computerName, const uint32_t type, std::span<const uint8_t> data, const DeliveryMode mode)
{
	const auto peer = resolvePeer(computerName);
	if (!peer.isValid())
		return false;

	// Application body: [u32 type][data]
	std::vector<uint8_t> body(sizeof(uint32_t) + data.size());
	channel::writeUint32(body.data(), type);
	std::ranges::copy(data, body.begin() + sizeof(uint32_t));

	if (mode == DeliveryMode::ReliableOrdered)
		return queueReliable(peer, channel::ChannelId::Application, std::move(body));

	if (!mInitialized.load() || !socket())
		return false;

	Batch batch;
	bool  sent = false;

	{
		std::lock_guard<std::mutex> lock(mLinksMutex);

		auto					   *link = linkFor(peer.address(), true);
		if (!link)
			return false;

		sent = link->sendUnreliable(channel::ChannelId::Application, body);
		collect(peer.address(), *link, batch, Clock::now());
	}

	execute(batch);
	return sent;
}


bool netlink::PeerChannel::queueReliable(const PeerEndpoint &peer, const channel::ChannelId channelId, std::vector<uint8_t> body)
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
		std::lock_guard<std::mutex> lock(mLinksMutex);

		auto					   *link = linkFor(peer.address(), true);
		if (!link)
			return false;

		const auto now = Clock::now();
		queued		   = link->queueReliable(channelId, std::move(body), now) != channel::PushResult::Rejected;
		collect(peer.address(), *link, batch, now);
	}

	execute(batch);
	return queued;
}


void netlink::PeerChannel::setKeepAlive(const std::string &computerName, const bool enabled)
{
	const auto peer = resolvePeer(computerName);
	if (!peer.isValid())
		return;

	std::lock_guard<std::mutex> lock(mLinksMutex);

	if (enabled)
	{
		// Heartbeats need a link, also towards a peer nothing was exchanged with yet
		linkFor(peer.address(), true);
		mHeartbeat.watch(peer.address(), Clock::now());
	}
	else
		mHeartbeat.unwatch(peer.address());
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

	const auto deadline = Clock::now() + timeout;

	while (true)
	{
		{
			std::lock_guard<std::mutex> lock(mLinksMutex);

			if (const auto *link = linkFor(peer.address(), false); !link || !link->hasPendingReliable())
				return true;
		}

		// Acknowledgements are only processed by the running I/O loop
		if (!isRunning() || Clock::now() >= deadline)
			return false;

		std::this_thread::sleep_for(std::chrono::milliseconds{2});
	}
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
		// Partial messages of the old stream can never complete
		mFragmentation.reset(address);
		mHeartbeat.unwatch(address);

		const char *reason = event == channel::LinkEvent::Failed ? "the peer stopped acknowledging messages" : "the peer restarted";
		batch.lostPeers.push_back({.address = address, .reason = reason});
	}

	auto datagrams = link.takeOutgoing();

	if (!datagrams.empty())
		mHeartbeat.onSent(address, now);

	for (auto &datagram : datagrams)
		batch.datagrams.push_back({.to = address, .bytes = std::move(datagram)});

	for (auto &[header, body] : link.takeDelivered())
	{
		if (auto message = mFragmentation.accept(address, header, body))
			batch.messages.push_back({.from = address, .message = std::move(*message)});
	}
}


// ---------------------------------------------------------------------------
// I/O loop
// ---------------------------------------------------------------------------

void netlink::PeerChannel::run()
{
	while (ThreadBase::isRunning())
	{
		receiveDatagram();
		serviceTimers();
	}
}


std::chrono::milliseconds netlink::PeerChannel::nextWait()
{
	std::optional<TimePoint> next;

	{
		std::lock_guard<std::mutex> lock(mLinksMutex);

		for (const auto &link : mLinks | std::views::values)
		{
			if (auto due = link->nextDeadline(); due && (!next || *due < *next))
				next = due;
		}

		if (const auto due = mHeartbeat.nextDeadline(); due && (!next || *due < *next))
			next = due;
	}

	if (!next)
		return internal::SocketPollInterval;

	const auto wait = std::chrono::ceil<std::chrono::milliseconds>(*next - Clock::now());
	return std::clamp(wait, std::chrono::milliseconds{1}, internal::SocketPollInterval);
}


void netlink::PeerChannel::receiveDatagram()
{
	const auto socket = mInitialized.load() ? this->socket() : nullptr;

	if (!socket)
	{
		waitForEvent(static_cast<unsigned long>(internal::SocketPollInterval.count()));
		return;
	}

	auto datagram = socket->receiveFrom(mReceiveBuffer, nextWait());

	if (!datagram)
	{
		// Timeouts are the normal idle case; anything else is retried on the next cycle
		if (datagram.error() != net::SocketError::Timeout)
			waitForEvent(static_cast<unsigned long>(internal::SocketPollInterval.count()));

		return;
	}

	handleDatagram(datagram->from, std::span<const uint8_t>(mReceiveBuffer.data(), datagram->size));
}


void netlink::PeerChannel::handleDatagram(const net::SocketAddress &from, const std::span<const uint8_t> bytes)
{
	const auto packet = channel::decodePacket(bytes);

	if (!packet)
	{
		NETLINK_LOG_DEBUG("Dropping malformed datagram from {} ({} bytes)", from.toString(), bytes.size());
		return;
	}

	Batch batch;

	{
		std::lock_guard<std::mutex> lock(mLinksMutex);

		auto					   *link = linkFor(from, true);
		if (!link)
			return;

		const auto now = Clock::now();
		link->onPacket(*packet, now);
		mHeartbeat.onReceived(from, now);
		collect(from, *link, batch, now);
	}

	execute(batch);
}


void netlink::PeerChannel::serviceTimers()
{
	Batch batch;

	{
		std::lock_guard<std::mutex> lock(mLinksMutex);

		const auto					now = Clock::now();

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
	}

	execute(batch);
}


// ---------------------------------------------------------------------------
// Carrying out collected work (no lock held)
// ---------------------------------------------------------------------------

void netlink::PeerChannel::execute(Batch &batch) const
{
	if (!batch.datagrams.empty())
	{
		if (const auto socket = this->socket())
		{
			for (const auto &[to, bytes] : batch.datagrams)
			{
				// A lost datagram is recovered by retransmission, a failed send is no different
				if (auto sent = socket->sendTo(to, bytes); !sent)
					NETLINK_LOG_DEBUG("Sending to {} failed: {}", to.toString(), net::toString(sent.error()));
			}
		}
	}

	for (auto &[from, message] : batch.messages)
	{
		if (message.channel == channel::ChannelId::Control)
			routeControl(from, message.body);
		else
			routeApplication(from, message.body);
	}

	for (const auto &[address, reason] : batch.lostPeers)
	{
		const std::string name = nameOf(address);
		NETLINK_LOG_WARNING("Lost peer {} ({}): {}", name.empty() ? "<unknown>" : name, address.toString(), reason);

		if (!name.empty() && mOnPeerLost)
			mOnPeerLost(name, reason);
	}
}


void netlink::PeerChannel::routeApplication(const net::SocketAddress &from, std::span<const uint8_t> body) const
{
	if (body.size() < sizeof(uint32_t))
	{
		NETLINK_LOG_WARNING("Dropping truncated application message from {}", from.toString());
		return;
	}

	const std::string name = nameOf(from);

	if (name.empty())
	{
		NETLINK_LOG_WARNING("Dropping application message from unknown remote {}", from.toString());
		return;
	}

	if (mMessageCallback)
		mMessageCallback(name, channel::readUint32(body.data()), std::vector<uint8_t>(body.begin() + sizeof(uint32_t), body.end()));
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
