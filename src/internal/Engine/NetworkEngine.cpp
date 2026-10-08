/*
  ==============================================================================
	Module:         NetworkEngine
	Description:    Everything that happens on the network: discovery, sessions
					and their links, on one UDP socket and one thread
  ==============================================================================
*/

#include "NetworkEngine.h"

#include <algorithm>
#include <random>
#include <ranges>

#include "NetLinkLog.h"
#include "NetLinkVersion.h"
#include "Session/ControlMessage.h"
#include "Socket/UdpSocket.h"

using netlink::channel::Mailbox;
using netlink::channel::SendScheduler;
using netlink::session::ControlType;


namespace
{

static_assert(netlink::MaxMessageSize == netlink::internal::MaxMessagePayload && netlink::MaxMediaMessageSize == netlink::internal::MaxMediaPayload);

// The longest the I/O thread waits without anything to do
constexpr auto	  MaxWait = std::chrono::hours{1};

constexpr uint8_t bitOf(const netlink::channel::Lane lane)
{
	return static_cast<uint8_t>(1u << std::to_underlying(lane));
}

constexpr netlink::channel::Lane wireLane(const netlink::Lane lane)
{
	switch (lane)
	{
	case netlink::Lane::Reliable: return netlink::channel::Lane::Reliable;
	case netlink::Lane::Bulk: return netlink::channel::Lane::Bulk;
	case netlink::Lane::Media: break;
	}

	return netlink::channel::Lane::Media;
}

constexpr netlink::Lane publicLane(const netlink::channel::Lane lane)
{
	return lane == netlink::channel::Lane::Bulk ? netlink::Lane::Bulk : lane == netlink::channel::Lane::Media ? netlink::Lane::Media : netlink::Lane::Reliable;
}

template <typename TimePoint>
std::optional<TimePoint> earlier(const std::optional<TimePoint> a, const std::optional<TimePoint> b)
{
	if (!a || !b)
		return a ? a : b;

	return std::min(*a, *b);
}

uint64_t makeInstanceId()
{
	std::random_device device;
	std::seed_seq	   seed{device(), device(), device(), device(), device(), device(), device(), device()};
	std::mt19937_64	   generator(seed);

	uint64_t		   id = 0;
	while (id == 0)
		id = generator();

	return id;
}

netlink::EngineConfig completed(netlink::EngineConfig config)
{
	if (config.displayName.size() > netlink::MaxDisplayName)
		config.displayName.resize(netlink::MaxDisplayName);

	if (config.appVersion.empty())
		config.appVersion = netlink::internal::Version;

	return config;
}

} // namespace


// Hands a link the messages that wait for its peer: session messages of the engine, and what the application queued
class netlink::NetworkEngine::PeerSource final : public channel::MessageSource
{
public:
	PeerSource(const NetworkEngine &engine, Mailbox &mailbox, Peer &peer) : mEngine(engine), mMailbox(mailbox), mPeer(peer) {}

	std::optional<channel::OutboundMessage> next(const WireLane lane) override
	{
		if (lane == WireLane::Control)
		{
			if (mPeer.control.empty())
				return std::nullopt;

			auto message = std::move(mPeer.control.front());
			mPeer.control.pop_front();
			return message;
		}

		if (!mEngine.transfersData(mPeer))
			return std::nullopt;

		auto mail = mMailbox.take(mPeer.info.id, lane);
		if (!mail)
			return std::nullopt;

		return channel::OutboundMessage{.tag = mail->tag, .body = std::move(mail->body)};
	}

private:
	const NetworkEngine &mEngine;
	Mailbox				&mMailbox;
	Peer				&mPeer;
};


std::optional<netlink::Message> netlink::EngineEvent::takeMessage()
{
	if (!media)
		return std::move(message);

	std::lock_guard<std::mutex> lock(media->mutex);

	if (media->waiting.empty())
		return std::nullopt;

	Message taken = std::move(media->waiting.front());
	media->waiting.pop_front();
	return taken;
}


netlink::NetworkEngine::BacklogShare::BacklogShare(std::shared_ptr<DeliveryBacklog> backlog, const size_t bytes) : backlog(std::move(backlog)), bytes(bytes)
{
	this->backlog->bytes.fetch_add(bytes);
}


netlink::NetworkEngine::BacklogShare::~BacklogShare()
{
	const size_t before = backlog->bytes.fetch_sub(bytes);

	// The moment the backlog falls below what senders are resumed at
	if (backlog->paused.load() && before >= channel::BacklogResumeBytes && before - bytes < channel::BacklogResumeBytes)
	{
		std::lock_guard<std::mutex> lock(backlog->mutex);

		if (backlog->onDrained)
			backlog->onDrained();
	}
}


netlink::NetworkEngine::NetworkEngine(EngineConfig config, net::DatagramSocketFactory socketFactory, LocalInterfaceProvider localInterface)
	: mConfig(completed(std::move(config))), mId{mConfig.instanceId != 0 ? mConfig.instanceId : makeInstanceId()},
	  mSocketFactory(socketFactory ? std::move(socketFactory) : net::UdpSocket::factory()), mLocalInterface(std::move(localInterface)), mMailbox([this] { wake(); }),
	  mDiscovery(
		  {.instanceId = mId.value, .appIdHash = discovery::hashAppId(mConfig.appId), .version = discovery::parseAppVersion(mConfig.appVersion), .name = mConfig.displayName}),
	  mScheduler(mConfig.maxSendRate), mReceiveBuffer(internal::PackageBufferSize)
{
	mMailbox.setQueueBytes(mConfig.sendQueueBytes);
	mDeliveryBacklog->onDrained = [this] { mMailbox.post(Mailbox::Command::ResumeReceiving); };
}


netlink::NetworkEngine::~NetworkEngine()
{
	mMailbox.setRunning(false);
	closeSockets();

	// Payload that is still waiting somewhere must not call into an engine that is gone
	std::lock_guard<std::mutex> lock(mDeliveryBacklog->mutex);
	mDeliveryBacklog->onDrained = nullptr;
}


// ---------------------------------------------------------------------------
// Any thread
// ---------------------------------------------------------------------------

netlink::net::SocketAddress netlink::NetworkEngine::localEndpoint() const
{
	std::lock_guard<std::mutex> lock(mSocketMutex);
	return mSocket ? mSocket->localAddress() : net::SocketAddress{};
}


void netlink::NetworkEngine::wake()
{
	std::shared_ptr<net::IDatagramSocket> socket;
	{
		std::lock_guard<std::mutex> lock(mSocketMutex);
		mWoken = true;
		socket = mSocket;
	}

	mWakeUp.notify_all();

	if (socket)
		socket->interrupt();
}


void netlink::NetworkEngine::setAnnouncing(const bool announcing)
{
	mMailbox.post(announcing ? Mailbox::Command::Announce : Mailbox::Command::StopAnnouncing);
}


void netlink::NetworkEngine::checkInterface()
{
	mMailbox.post(Mailbox::Command::CheckInterface);
}


bool netlink::NetworkEngine::connect(const PeerId peer)
{
	if (!mMailbox.isRunning())
		return false;

	// A peer that asked itself is answered with it. Otherwise only a peer without a session can be asked.
	const auto session = mMailbox.session(peer);

	if (session != Mailbox::Session::Requested && (session != Mailbox::Session::None || !mMailbox.isDiscovered(peer)))
		return false;

	mMailbox.post(Mailbox::Command::Connect, peer);
	return true;
}


void netlink::NetworkEngine::accept(const PeerId peer)
{
	mMailbox.post(Mailbox::Command::Accept, peer);
}


void netlink::NetworkEngine::decline(const PeerId peer)
{
	mMailbox.post(Mailbox::Command::Decline, peer);
}


void netlink::NetworkEngine::disconnect(const PeerId peer)
{
	mMailbox.seal(peer);
	mMailbox.post(Mailbox::Command::Disconnect, peer);
}


netlink::SendResult netlink::NetworkEngine::send(const PeerId peer, const uint32_t type, std::vector<uint8_t> &&data, const Lane lane, const std::chrono::milliseconds timeout)
{
	// The type travels as the tag of the message: the payload stays in the buffer its fragments are sent from
	Mailbox::Mail mail{.tag = type, .body = std::make_shared<const std::vector<uint8_t>>(std::move(data))};

	switch (mMailbox.push(peer, wireLane(lane), std::move(mail), timeout))
	{
	case Mailbox::Push::Queued: return SendResult::Queued;
	case Mailbox::Push::Full: return SendResult::QueueFull;
	case Mailbox::Push::TooLarge: return SendResult::TooLarge;
	case Mailbox::Push::Closed: return SendResult::NotConnected;
	case Mailbox::Push::Stopped: break;
	}

	return SendResult::NotRunning;
}


size_t netlink::NetworkEngine::broadcast(const uint32_t type, const std::span<const uint8_t> data, const Lane lane)
{
	// One body, shared by the messages to every peer
	return mMailbox.pushToAll(wireLane(lane), {.tag = type, .body = std::make_shared<const std::vector<uint8_t>>(data.begin(), data.end())});
}


bool netlink::NetworkEngine::flush(const PeerId peer, const std::chrono::milliseconds timeout)
{
	return mMailbox.flush(peer, timeout);
}


void netlink::NetworkEngine::shutdown()
{
	mMailbox.post(Mailbox::Command::Shutdown);
}


// ---------------------------------------------------------------------------
// I/O loop
// ---------------------------------------------------------------------------

void netlink::NetworkEngine::run(const std::stop_token &stop, const std::function<void(EventBatch &&)> &deliver)
{
	const std::stop_callback wakeOnStop(stop, [this] { wake(); });

	try
	{
		while (!stop.stop_requested() && !finished())
		{
			const auto now = Clock::now();

			// How long a round takes that only waited for the next tick of the send budget
			if (std::exchange(mTickTimedOut, false))
			{
				mBudgetTicks.fetch_add(1, std::memory_order_relaxed);
				mBudgetTickTime.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - mStepStartedAt).count()), std::memory_order_relaxed);
			}

			mStepStartedAt	 = now;
			EventBatch batch = step(now);

			if (!batch.events.empty())
				deliver(std::move(batch));

			if (!finished())
				waitForWork();
		}
	}
	catch (const std::exception &e)
	{
		NETLINK_LOG_ERROR("The I/O loop of the engine ended with an exception: {}", e.what());
		fail(deliver);
	}
	catch (...)
	{
		NETLINK_LOG_ERROR("The I/O loop of the engine ended with an unknown exception");
		fail(deliver);
	}

	// Nothing is sent or acknowledged anymore: whoever waits for it gives up
	mMailbox.setRunning(false);
}


void netlink::NetworkEngine::fail(const std::function<void(EventBatch &&)> &deliver)
{
	mEvents.clear();
	endAllSessions(DisconnectReason::NetworkError);
	mMailbox.setRunning(false);

	if (EventBatch batch = takeBatch(); !batch.events.empty())
		deliver(std::move(batch));
}


void netlink::NetworkEngine::waitForWork()
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

	{
		std::unique_lock<std::mutex> lock(mSocketMutex);

		// Work that arrived during the step may have rung at a socket that was replaced since, or at none at all
		if (mWoken)
			return;

		if (!mIoSocket)
		{
			mWakeUp.wait_for(lock, timeout, [this] { return mWoken; });
			return;
		}
	}

	const auto ready = mIoSocket->waitReadable(timeout);
	mTickTimedOut	 = mWaitingForTick && !ready && ready.error() == net::SocketError::Timeout;

	// A socket that cannot even be waited on is replaced with the next look at the adapter
	if (!ready && ready.error() != net::SocketError::Timeout && ready.error() != net::SocketError::Cancelled)
		mSocketFailed = true;
}


netlink::EventBatch netlink::NetworkEngine::takeBatch()
{
	EventBatch batch;
	batch.events.swap(mEvents);

	// What waits for the application is accounted for until its batch is gone. Media is not: it is neither paused nor kept.
	size_t bytes = 0;
	for (const auto &event : batch.events)
		bytes += event.kind == EngineEvent::Kind::Message ? event.message.data.size() : 0;

	if (bytes > 0)
		batch.backlog = std::make_shared<BacklogShare>(mDeliveryBacklog, bytes);

	return batch;
}


netlink::EventBatch netlink::NetworkEngine::step(const TimePoint now)
{
	mSteps.fetch_add(1, std::memory_order_relaxed);

	{
		std::lock_guard<std::mutex> lock(mSocketMutex);
		mWoken = false;
	}

	// Peers whose acknowledgements the socket did not take in the last step get their turn again
	mTouched.clear();
	mTouched.swap(mAcksHeldBack);

	mMailbox.drain(mWork);

	for (const auto &command : mWork.commands)
		apply(command, now);

	for (const auto &id : mWork.ready)
	{
		if (Peer *peer = find(id))
		{
			peer->unsettled = true;
			touch(id);
		}
	}

	const bool tickDue = !mShutdown && (!mNextTick || now >= *mNextTick);

	if (!mShutdown && (tickDue || mCheckInterface || mSocketFailed))
		updateInterface(std::exchange(mSocketFailed, false));

	updatePause();
	receivePending(now);
	serviceTimers(now);

	if (tickDue)
		tick(now);

	if (std::exchange(mBeaconDue, false) && mDiscovery.isAnnouncing() && !mShutdown)
		sendBeacon({.ip = mDiscovery.broadcastAddress(), .port = mConfig.discoveryPort}, false);

	std::ranges::sort(mTouched);
	const auto duplicates = std::ranges::unique(mTouched);
	mTouched.erase(duplicates.begin(), duplicates.end());

	mScheduler.refill(now);
	mSocketBlocked = false;

	for (const auto &id : mTouched)
	{
		Peer *peer = find(id);
		if (!peer)
			continue;

		advance(*peer, now);
		sendAcks(*peer);

		if (!peer->ended)
			schedule(*peer, now);
	}

	if (!mSocketBlocked)
		sendData(now);

	for (const auto &id : mTouched)
	{
		Peer *peer = find(id);
		if (!peer || peer->ended)
			continue;

		peer->deadline = peer->link.nextDeadline();

		if (peer->unsettled && !peer->link.hasPendingReliable())
			peer->unsettled = !mMailbox.settle(id);

		if (peer->state == Peer::State::Connected)
			mMailbox.publishStats(id, statsOf(*peer));
	}

	for (const auto &id : std::exchange(mEnded, {}))
	{
		// Not a session that was opened under the same ID in the meantime
		if (const Peer *peer = find(id); peer && peer->ended)
			erase(id);
	}

	if (std::exchange(mPeersChanged, false))
		mMailbox.publishDiscovered(mDiscovery.peers());

	mNextWake = mShutdown ? std::nullopt : mNextTick;

	for (const auto &peer : mPeers | std::views::values)
		mNextWake = earlier(earlier(mNextWake, peer.deadline), peer.stateDeadline);

	// Datagrams wait for tokens or for the socket
	mWaitingForTick = mSocketBlocked || mScheduler.hasBacklog() || !mAcksHeldBack.empty();

	if (mWaitingForTick || mSocketFailed)
		mNextWake = earlier(mNextWake, std::optional{now + SendScheduler::Tick});

	return takeBatch();
}


// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

void netlink::NetworkEngine::apply(const Mailbox::PostedCommand &command, const TimePoint now)
{
	const auto &[kind, id] = command;
	Peer *peer			   = find(id);

	switch (kind)
	{
	case Mailbox::Command::Connect: startSession(id, now); break;

	case Mailbox::Command::Accept:
		if (peer && peer->state == Peer::State::AwaitingDecision)
			sendAccept(*peer);
		break;

	case Mailbox::Command::Decline:
		if (peer && peer->state == Peer::State::AwaitingDecision)
			sendDecline(*peer, now);
		break;

	case Mailbox::Command::Disconnect:
		if (peer)
			endSession(*peer, now);
		break;

	case Mailbox::Command::Announce:
		mDiscovery.setAnnouncing(true);
		mBeaconDue = true;
		break;

	case Mailbox::Command::StopAnnouncing: mDiscovery.setAnnouncing(false); break;
	case Mailbox::Command::CheckInterface: mCheckInterface = true; break;

	// Only wakes the loop: every step looks at what waits for the application
	case Mailbox::Command::ResumeReceiving: break;

	case Mailbox::Command::Shutdown: beginShutdown(now); break;
	}
}


void netlink::NetworkEngine::startSession(const PeerId id, const TimePoint now)
{
	if (mShutdown)
		return;

	if (Peer *existing = find(id))
	{
		// The peer asked first: connecting to it is the answer
		if (existing->state == Peer::State::AwaitingDecision)
		{
			sendAccept(*existing);
			return;
		}

		// A session that only waits for its goodbye to be acknowledged makes room for the new one
		if (existing->state != Peer::State::Closing || !existing->closeSent)
			return;

		erase(id);
	}

	const discovery::KnownPeer *known = mDiscovery.find(id);

	if (!known || !mIoSocket || mPeers.size() >= MaxPeers)
	{
		emit({.kind = EngineEvent::Kind::Disconnected, .peer = id, .reason = known ? DisconnectReason::Local : DisconnectReason::Lost});
		return;
	}

	Peer &peer			 = addPeer(known->info, known->endpoint, now);
	peer.initiator		 = true;
	peer.announced		 = true;

	const auto &identity = mDiscovery.identity();
	const auto	hello	 = session::encode(session::Hello{.fromInstance = mId.value,
														  .toInstance	= id.value,
														  .appIdHash	= identity.appIdHash,
														  .verMajor		= identity.version.verMajor,
														  .verMinor		= identity.version.verMinor,
														  .name			= identity.name});

	peer.control.push_back({.tag = std::to_underlying(ControlType::Hello), .body = std::make_shared<const std::vector<uint8_t>>(hello)});
	mMailbox.publishSession(id, Mailbox::Session::Pending);
}


void netlink::NetworkEngine::endSession(Peer &peer, const TimePoint now)
{
	switch (peer.state)
	{
	// What is waiting is still sent, for a while: the goodbye follows it
	case Peer::State::Connected:
		peer.state		   = Peer::State::Closing;
		peer.stateDeadline = now + internal::DrainTimeout;
		touch(peer.info.id);
		break;

	case Peer::State::AwaitingDecision: sendDecline(peer, now); break;

	case Peer::State::Connecting:
		report(peer, DisconnectReason::Local);
		sendClose(peer, now);
		break;

	case Peer::State::Closing: break;
	}
}


void netlink::NetworkEngine::beginShutdown(const TimePoint now)
{
	if (std::exchange(mShutdown, true))
		return;

	mDiscovery.setAnnouncing(false);

	for (auto &peer : mPeers | std::views::values)
	{
		report(peer, DisconnectReason::Shutdown);

		if (peer.state == Peer::State::AwaitingDecision)
			sendDecline(peer, now);
		else if (!peer.closeSent)
			sendClose(peer, now);
	}

	mMailbox.setRunning(false);
}


// ---------------------------------------------------------------------------
// The adapter and its sockets
// ---------------------------------------------------------------------------

void netlink::NetworkEngine::updateInterface(const bool socketFailed)
{
	mCheckInterface	   = false;

	const auto current = mLocalInterface ? mLocalInterface() : std::optional<LocalInterface>{};
	const bool changed = current != mInterface;

	if (!changed && !socketFailed)
	{
		// Without a socket although the adapter has an address: binding failed, or the socket broke
		if (current && !mIoSocket)
			bind(*current);

		return;
	}

	if (mIoSocket)
	{
		NETLINK_LOG_WARNING("The network adapter {}: every session ends", changed ? "changed" : "stopped working");
		endAllSessions(changed ? DisconnectReason::Local : DisconnectReason::NetworkError);
	}

	for (const auto &id : mDiscovery.clear())
		emit({.kind = EngineEvent::Kind::PeerLost, .peer = id});

	mPeersChanged = true;
	mInterface	  = current;
	closeSockets();

	// A socket that broke on an adapter that looks unchanged is replaced with the next tick, not in a loop
	if (current && changed)
		bind(*current);
}


void netlink::NetworkEngine::bind(const LocalInterface &iface)
{
	// Room for one burst of the send budget, where the operating system needs to be asked for it
	const auto burst  = static_cast<int>(SendScheduler::burstOf(mConfig.maxSendRate > 0 ? mConfig.maxSendRate : channel::DefaultMaxSendRate));

	auto	   socket = mSocketFactory({.ip = iface.ip, .port = 0}, {.enableBroadcast	= true,
																	 .reuseAddress		= false,
																	 .receiveBufferSize = internal::ChannelReceiveBufferSize,
																	 .sendBufferSize	= burst * internal::ChannelSendBufferPerDatagram});

	if (!socket)
	{
		NETLINK_LOG_ERROR("Failed to bind the engine's socket to {}: {}", iface.ip.toString(), net::toString(socket.error()));
		return;
	}

	// Announcements of other peers arrive here. Several engines on one machine share the port.
	auto beacons = mSocketFactory(net::SocketAddress::any(mConfig.discoveryPort), {.enableBroadcast = false, .reuseAddress = true, .receiveBufferSize = 0, .sendBufferSize = 0});

	if (beacons)
		mBeaconSocket = std::move(*beacons);
	else
		NETLINK_LOG_WARNING("Failed to bind the discovery port {}: {}. Peers are only found once they answer.", mConfig.discoveryPort, net::toString(beacons.error()));

	{
		std::lock_guard<std::mutex> lock(mSocketMutex);
		mSocket	  = std::move(*socket);
		mIoSocket = mSocket.get();
	}

	mDiscovery.setInterface(iface.ip, iface.mask);
	mBeaconDue = true;

	NETLINK_LOG_INFO("Engine bound to {}", mIoSocket->localAddress().toString());
	emit({.kind = EngineEvent::Kind::AdapterChanged, .info = {.address = iface.ip.toString(), .port = mIoSocket->localAddress().port}});
}


void netlink::NetworkEngine::closeSockets()
{
	std::shared_ptr<net::IDatagramSocket> previous;
	{
		std::lock_guard<std::mutex> lock(mSocketMutex);
		previous  = std::exchange(mSocket, nullptr);
		mIoSocket = nullptr;
	}

	if (previous)
		previous->shutdown();

	if (mBeaconSocket)
		mBeaconSocket->shutdown();

	mBeaconSocket.reset();
}


void netlink::NetworkEngine::endAllSessions(const DisconnectReason reason)
{
	for (auto &[id, peer] : mPeers)
	{
		report(peer, reason);
		mMailbox.close(id);
	}

	mPeers.clear();
	mByStream.clear();
	mScheduler.clear();
	mAcksHeldBack.clear();
	mEnded.clear();
}


// ---------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------

void netlink::NetworkEngine::receivePending(const TimePoint now)
{
	if (!mIoSocket)
		return;

	// Everything that is waiting is taken in before anything is answered: one send pass and one hand-over for all of it
	for (size_t received = 0; received < MaxDatagramsPerPass; ++received)
	{
		const auto datagram = mIoSocket->receiveFrom(mReceiveBuffer, std::chrono::microseconds::zero());

		// Larger than any datagram of an engine: the operating system dropped it, whatever waits behind it is still read
		if (!datagram && datagram.error() == net::SocketError::MessageTooLarge)
			continue;

		if (!datagram)
			break;

		handleDatagram(datagram->from, std::span<const uint8_t>(mReceiveBuffer.data(), datagram->size), now);
	}

	// The engine never waits on the discovery port: what arrived there is taken along with every step
	for (size_t received = 0; mBeaconSocket && received < MaxDatagramsPerPass; ++received)
	{
		const auto datagram = mBeaconSocket->receiveFrom(mReceiveBuffer, std::chrono::microseconds::zero());

		if (!datagram && datagram.error() == net::SocketError::MessageTooLarge)
			continue;

		if (!datagram)
			break;

		onBeacon(datagram->from, std::span<const uint8_t>(mReceiveBuffer.data(), datagram->size), now);
	}
}


void netlink::NetworkEngine::handleDatagram(const net::SocketAddress &from, const std::span<const uint8_t> bytes, const TimePoint now)
{
	if (discovery::isBeacon(bytes))
	{
		onBeacon(from, bytes, now);
		return;
	}

	const auto packet = channel::decodePacket(bytes);

	if (!packet)
	{
		NETLINK_LOG_DEBUG("Dropping malformed datagram from {} ({} bytes)", from.toString(), bytes.size());
		return;
	}

	const auto &header = packet->header;
	Peer	   *peer   = nullptr;

	if (header.dstStreamID == 0)
	{
		// Only the first packet of a link may not know whom it talks to
		if (header.flags.kind() != channel::PacketKind::Data || header.flags.lane() != WireLane::Control || header.seq != 1 || header.flags.isFragmented() ||
			header.tag != std::to_underlying(ControlType::Hello))
			return;

		peer = onHello(from, *packet, now);
	}
	else if (const auto it = mByStream.find(header.dstStreamID); it != mByStream.end())
	{
		peer = find(it->second);
	}

	if (!peer || peer->ended || peer->address != from)
		return;

	// Before a session is connected nothing but its Control lane is taken, and nothing else is acknowledged
	if (header.flags.lane() != WireLane::Control && !transfersData(*peer))
	{
		// The Accept arrived: the remote already uses the session
		if (peer->state != Peer::State::Connecting || peer->initiator || !peer->acceptSent)
			return;

		setConnected(*peer);
	}

	peer->link.onPacket(*packet, now);
	touch(peer->info.id);
}


void netlink::NetworkEngine::onBeacon(const net::SocketAddress &from, const std::span<const uint8_t> bytes, const TimePoint now)
{
	if (mShutdown)
		return;

	const auto seen = mDiscovery.onBeacon(from, bytes, now);

	if (seen.sighting == discovery::DiscoveryLogic::Sighting::New)
	{
		emit({.kind = EngineEvent::Kind::PeerDiscovered, .peer = seen.peer->info.id, .info = seen.peer->info});
		mPeersChanged = true;
	}

	if (seen.reply)
		sendBeacon(seen.peer->endpoint, true);
}


netlink::NetworkEngine::Peer *netlink::NetworkEngine::onHello(const net::SocketAddress &from, const channel::DecodedPacket &packet, const TimePoint now)
{
	// Sent again because the answer is still on its way: its link exists
	for (auto &peer : mPeers | std::views::values)
	{
		if (peer.address == from && peer.link.remoteStreamID() == packet.header.srcStreamID)
			return &peer;
	}

	const auto	hello	 = session::decodeHello(packet.body);
	const auto &identity = mDiscovery.identity();

	// Everything is checked before any state is created for the sender
	if (mShutdown || !hello || hello->toInstance != mId.value || hello->fromInstance == mId.value || hello->appIdHash != identity.appIdHash ||
		discovery::AppVersion{hello->verMajor, hello->verMinor} != identity.version || hello->name.size() > MaxDisplayName || !mDiscovery.isInSubnet(from.ip))
		return nullptr;

	const PeerId id{hello->fromInstance};
	bool		 crossing = false;

	if (Peer *existing = find(id))
	{
		if (existing->state == Peer::State::Connecting && existing->initiator)
		{
			// Both asked at once: the one with the higher ID keeps asking, the other one answers
			if (mId > id)
				return nullptr;

			crossing = true;
		}
		else
		{
			// The remote started over: it dropped its side of the session
			report(*existing, DisconnectReason::Lost);
		}

		erase(id);
	}

	const auto pending = std::ranges::count_if(mPeers | std::views::values, [](const Peer &peer) { return peer.state != Peer::State::Connected; });

	if (mPeers.size() >= MaxPeers || static_cast<size_t>(pending) >= internal::MaxPendingSessions)
		return nullptr;

	const discovery::AppVersion version{hello->verMajor, hello->verMinor};
	Peer &peer = addPeer({.id = id, .displayName = hello->name, .address = from.ip.toString(), .port = from.port, .appVersion = version.toString()}, from, now);

	if (crossing || mConfig.autoAccept)
	{
		// Its own application asked for this session: nobody has to be asked again
		peer.announced = crossing;
		mMailbox.publishSession(id, Mailbox::Session::Pending);
		sendAccept(peer);
	}
	else
	{
		peer.state		   = Peer::State::AwaitingDecision;
		peer.stateDeadline = now + internal::DecisionTimeout;
		peer.announced	   = true;
		mMailbox.publishSession(id, Mailbox::Session::Requested);
		emit({.kind = EngineEvent::Kind::ConnectionRequest, .peer = id, .info = peer.info});
	}

	return &peer;
}


// ---------------------------------------------------------------------------
// Timers
// ---------------------------------------------------------------------------

void netlink::NetworkEngine::serviceTimers(const TimePoint now)
{
	for (auto &[id, peer] : mPeers)
	{
		if (peer.deadline && *peer.deadline <= now)
		{
			peer.link.onTimer(now);
			touch(id);
		}

		if (peer.stateDeadline && *peer.stateDeadline <= now)
			touch(id);
	}
}


void netlink::NetworkEngine::tick(const TimePoint now)
{
	mNextTick  = now + internal::BeaconInterval;
	mBeaconDue = true;

	for (const auto &id : mDiscovery.expire(now, [this](const PeerId peer) { return mPeers.contains(peer); }))
	{
		emit({.kind = EngineEvent::Kind::PeerLost, .peer = id});
		mPeersChanged = true;
	}
}


void netlink::NetworkEngine::sendBeacon(const net::SocketAddress &to, const bool reply)
{
	if (!mIoSocket)
		return;

	// Not held back by the budget, but counted against it
	if (const auto sent = mIoSocket->sendTo(to, mDiscovery.beacon(reply)); sent)
	{
		mDatagramsSent.fetch_add(1, std::memory_order_relaxed);
		mScheduler.spend();
	}
	else
	{
		NETLINK_LOG_DEBUG("Announcing to {} failed: {}", to.toString(), net::toString(sent.error()));
	}
}


void netlink::NetworkEngine::updatePause()
{
	// Too much waits for the application: every sender is asked to hold back, and told when there is room again
	const size_t waiting = mDeliveryBacklog->bytes.load();

	if (mPaused ? waiting >= channel::BacklogResumeBytes : waiting <= channel::BacklogPauseBytes)
		return;

	mPaused = !mPaused;
	mDeliveryBacklog->paused.store(mPaused);

	for (auto &[id, peer] : mPeers)
	{
		peer.link.setPaused(mPaused);
		touch(id);
	}
}


// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------

bool netlink::NetworkEngine::transfersData(const Peer &peer) const
{
	return peer.state == Peer::State::Connected || (peer.state == Peer::State::Closing && !peer.closeSent);
}


void netlink::NetworkEngine::advance(Peer &peer, const TimePoint now)
{
	if (peer.ended)
		return;

	if (peer.link.hasFailed())
	{
		report(peer, DisconnectReason::Lost);
		end(peer);
		return;
	}

	for (auto &message : peer.link.takeDelivered())
	{
		if (message.lane == WireLane::Control)
		{
			onControl(peer, message);
			continue;
		}

		if (peer.ended || peer.reported || !transfersData(peer))
			continue;

		EngineEvent event{.kind = EngineEvent::Kind::Message, .peer = peer.info.id, .lane = publicLane(message.lane)};

		if (message.lane == WireLane::Media)
		{
			if (!peer.media)
				peer.media = std::make_shared<MediaInbox>();

			std::lock_guard<std::mutex> lock(peer.media->mutex);

			// The application is behind: the oldest message it did not take yet makes room
			if (peer.media->waiting.size() >= channel::MediaQueueMessages)
				peer.media->waiting.pop_front();

			peer.media->waiting.push_back({.type = message.tag, .data = std::move(message.body)});
			event.media = peer.media;
		}
		else
		{
			event.message = {.type = message.tag, .data = std::move(message.body)};
		}

		emit(std::move(event));
	}

	if (peer.ended)
		return;

	const bool controlSettled = peer.control.empty() && !peer.link.hasPending(WireLane::Control);
	const bool expired		  = peer.stateDeadline && *peer.stateDeadline <= now;

	switch (peer.state)
	{
	case Peer::State::Connecting:
		// The remote has the Accept: the session is open on this side as well
		if (!peer.initiator && peer.acceptSent && controlSettled)
			setConnected(peer);
		break;

	case Peer::State::AwaitingDecision:
		if (expired)
			sendDecline(peer, now);
		break;

	case Peer::State::Connected: break;

	case Peer::State::Closing:
		if (peer.closeSent)
		{
			if (controlSettled || expired)
				end(peer);
		}
		else if (expired || (!peer.link.hasPending(WireLane::Reliable) && !peer.link.hasPending(WireLane::Bulk) && mMailbox.settle(peer.info.id)))
		{
			report(peer, DisconnectReason::Local);
			sendClose(peer, now);
		}
		break;
	}
}


void netlink::NetworkEngine::onControl(Peer &peer, const channel::DeliveredMessage &message)
{
	if (peer.ended)
		return;

	switch (static_cast<ControlType>(message.tag))
	{
	// Served when its link was created
	case ControlType::Hello: break;

	case ControlType::Accept:
		if (peer.state == Peer::State::Connecting && peer.initiator)
		{
			if (const auto accept = session::decodeAccept(message.body); accept && !accept->name.empty() && accept->name.size() <= MaxDisplayName)
				peer.info.displayName = accept->name;

			setConnected(peer);
		}
		break;

	case ControlType::Decline:
		if (peer.state == Peer::State::Connecting && peer.initiator)
		{
			const auto decline = session::decodeDecline(message.body);
			report(peer, decline && decline->reason == session::DeclineReason::Incompatible ? DisconnectReason::Incompatible : DisconnectReason::Declined);
			end(peer);
		}
		break;

	case ControlType::Close:
		report(peer, DisconnectReason::Remote);
		end(peer);
		break;

	default: NETLINK_LOG_DEBUG("Ignoring unknown session message {} from {}", message.tag, peer.address.toString()); break;
	}
}


void netlink::NetworkEngine::setConnected(Peer &peer)
{
	peer.state	   = Peer::State::Connected;
	peer.announced = true;
	peer.stateDeadline.reset();

	mMailbox.open(peer.info.id);
	emit({.kind = EngineEvent::Kind::Connected, .peer = peer.info.id, .info = peer.info});
}


void netlink::NetworkEngine::sendAccept(Peer &peer)
{
	const auto accept = session::encode(session::Accept{.name = mDiscovery.identity().name});

	peer.control.push_back({.tag = std::to_underlying(ControlType::Accept), .body = std::make_shared<const std::vector<uint8_t>>(accept)});
	peer.state		= Peer::State::Connecting;
	peer.acceptSent = true;
	peer.stateDeadline.reset();

	mMailbox.publishSession(peer.info.id, Mailbox::Session::Pending);
	touch(peer.info.id);
}


void netlink::NetworkEngine::sendDecline(Peer &peer, const TimePoint now)
{
	const auto decline = session::encode(session::Decline{.reason = session::DeclineReason::Declined, .text = {}});

	peer.control.push_back({.tag = std::to_underlying(ControlType::Decline), .body = std::make_shared<const std::vector<uint8_t>>(decline)});
	report(peer, DisconnectReason::Local);

	peer.state		   = Peer::State::Closing;
	peer.closeSent	   = true;
	peer.stateDeadline = now + internal::CloseLinger;

	mMailbox.close(peer.info.id);
	touch(peer.info.id);
}


void netlink::NetworkEngine::sendClose(Peer &peer, const TimePoint now)
{
	mMailbox.close(peer.info.id);

	// A remote that never answered does not know this link: there is nobody to tell
	if (!peer.link.remoteStreamID())
	{
		end(peer);
		return;
	}

	peer.control.push_back({.tag = std::to_underlying(ControlType::Close), .body = std::make_shared<const std::vector<uint8_t>>(session::encodeClose())});
	peer.state		   = Peer::State::Closing;
	peer.closeSent	   = true;
	peer.stateDeadline = now + internal::CloseLinger;
	touch(peer.info.id);
}


void netlink::NetworkEngine::report(Peer &peer, const DisconnectReason reason)
{
	if (!peer.announced || std::exchange(peer.reported, true))
		return;

	emit({.kind = EngineEvent::Kind::Disconnected, .peer = peer.info.id, .reason = reason});
}


void netlink::NetworkEngine::end(Peer &peer)
{
	if (std::exchange(peer.ended, true))
		return;

	mMailbox.close(peer.info.id);
	mEnded.push_back(peer.info.id);
}


void netlink::NetworkEngine::erase(const PeerId id)
{
	const auto it = mPeers.find(id);
	if (it == mPeers.end())
		return;

	mByStream.erase(it->second.link.localStreamID());
	mScheduler.remove(id);
	mMailbox.close(id);
	mPeers.erase(it);
}


netlink::NetworkEngine::Peer *netlink::NetworkEngine::find(const PeerId id)
{
	const auto it = mPeers.find(id);
	return it != mPeers.end() ? &it->second : nullptr;
}


netlink::NetworkEngine::Peer &netlink::NetworkEngine::addPeer(PeerInfo info, const net::SocketAddress &address, const TimePoint now)
{
	// What arrives for this link is found by its stream ID
	uint32_t streamID = channel::makeStreamID();
	while (mByStream.contains(streamID))
		streamID = channel::makeStreamID();

	const PeerId id	  = info.id;
	Peer		&peer = mPeers.try_emplace(id, std::move(info), address, mConfig.timings, streamID, &mAssemblyBudget).first->second;

	peer.link.supervise(now);
	peer.link.setPaused(mPaused);
	mByStream[streamID] = id;

	touch(id);
	return peer;
}


netlink::PeerStats netlink::NetworkEngine::statsOf(Peer &peer) const
{
	const auto &link = peer.link.stats();

	// The share of the Media datagrams that got lost between the two latest reports of the remote
	if (link.mediaReceivedByPeer != peer.mediaReceivedAtReport)
	{
		const auto sent		= static_cast<double>(link.mediaSent - peer.mediaSentAtReport);
		const auto received = static_cast<double>(link.mediaReceivedByPeer - peer.mediaReceivedAtReport);

		if (sent > 0.0)
			peer.mediaLoss = static_cast<float>(std::clamp(1.0 - received / sent, 0.0, 1.0));

		peer.mediaSentAtReport	   = link.mediaSent;
		peer.mediaReceivedAtReport = link.mediaReceivedByPeer;
	}

	return {.rtt			 = peer.link.rtt().srtt(),
			.bytesQueued	 = 0,
			.bytesSent		 = link.bytesSent,
			.bytesReceived	 = link.bytesReceived,
			.retransmissions = link.retransmissions,
			.mediaSent		 = link.mediaSent,
			.mediaDropped	 = 0,
			.mediaIncomplete = peer.link.mediaIncomplete(),
			.mediaLoss		 = peer.mediaLoss};
}


// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

void netlink::NetworkEngine::sendAcks(Peer &peer)
{
	// Not held back by the budget, but counted against it
	while (const auto *ack = peer.link.peekAck())
	{
		if (mSocketBlocked || transmit(peer.address, *ack) == Transmit::Refused)
		{
			mAcksHeldBack.push_back(peer.info.id);
			return;
		}

		peer.link.commitAck();
		mScheduler.spend();
	}
}


void netlink::NetworkEngine::schedule(Peer &peer, const TimePoint now)
{
	PeerSource source(*this, mMailbox, peer);

	for (const WireLane lane : SendScheduler::Order)
	{
		if ((peer.scheduled & bitOf(lane)) == 0 && peer.link.peek(lane, now, source))
		{
			mScheduler.add(lane, peer.info.id);
			peer.scheduled |= bitOf(lane);
		}
	}
}


void netlink::NetworkEngine::sendData(const TimePoint now)
{
	mScheduler.run(
		[&](const WireLane lane, const PeerId &id)
		{
			Peer *peer = find(id);
			if (!peer)
				return SendScheduler::Result::Empty;

			PeerSource	source(*this, mMailbox, *peer);
			const auto *datagram = peer->link.peek(lane, now, source);

			if (!datagram)
			{
				peer->scheduled &= static_cast<uint8_t>(~bitOf(lane));
				return SendScheduler::Result::Empty;
			}

			const Transmit outcome = transmit(peer->address, *datagram);
			if (outcome == Transmit::Refused)
				return SendScheduler::Result::Blocked;

			peer->link.commit(lane, now);
			peer->deadline = peer->link.nextDeadline();

			return outcome == Transmit::Sent ? SendScheduler::Result::Sent : SendScheduler::Result::Lost;
		});
}


netlink::NetworkEngine::Transmit netlink::NetworkEngine::transmit(const net::SocketAddress &address, const channel::OutgoingDatagram &datagram)
{
	if (!mIoSocket)
		return Transmit::Lost;

	const auto sent = mIoSocket->sendParts(address, datagram.header(), datagram.body());

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
		NETLINK_LOG_WARNING("The engine's socket cannot send anymore: {}", net::toString(sent.error()));
		mSocketBlocked = true;
		mSocketFailed  = true;
		return Transmit::Refused;

	default:
		// The destination cannot be reached right now: no different from a datagram that got lost on the way
		NETLINK_LOG_DEBUG("Sending to {} failed: {}", address.toString(), net::toString(sent.error()));
		return Transmit::Lost;
	}
}
