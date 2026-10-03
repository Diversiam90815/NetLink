/*
  ==============================================================================
	Module:         PeerChannel
	Description:    The dedicated UDP socket all peer-to-peer traffic runs on
  ==============================================================================
*/

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "SignalPacket.h"
#include "TaskQueue.h"
#include "ThreadBase.h"
#include "Heartbeat/HeartbeatService.h"
#include "PeerValidation/PeerValidationService.h"
#include "Reliability/ReliableLink.h"
#include "Socket/IDatagramSocket.h"


namespace netlink
{

// Connection lifecycle signals (consumed by ConnectionService)
struct ChannelConnectionCallbacks
{
	std::function<void(const std::string &computerName)>										   onConnectRequested;
	std::function<void(const std::string &computerName, bool accepted, const std::string &reason)> onConnectRequestAnswered;
	std::function<void(const std::string &computerName)>										   onDisconnectReceived;
	std::function<void(const std::string &computerName)>										   onReadyFlagReceived;
};


// Peer validation signals (consumed by PeerValidationService)
struct ChannelValidationCallbacks
{
	std::function<void(const std::string &computerName, RemoteRequest request)>		 onValidationRequestReceived;
	std::function<void(const std::string &computerName, const std::string &secret)>	 onSecretResponseReceived;
	std::function<void(const std::string &computerName, const std::string &version)> onVersionResponseReceived;
	std::function<void(const std::string &computerName)>							 onValidationHandshakeReceived;
};


using SocketBoundCallback	 = std::function<void(int boundPort)>;
using ChannelMessageCallback = std::function<void(const std::string &computerName, uint32_t type, std::vector<uint8_t> data)>;
using PeerLostCallback		 = std::function<void(const std::string &computerName, const std::string &reason)>;


struct PeerEndpoint
{
	net::IPv4Address   IPv4{};
	int				   channelPort{0};

	bool			   isValid() const { return !IPv4.isUnspecified() && channelPort != 0; }
	net::SocketAddress address() const { return {.ip = IPv4, .port = static_cast<uint16_t>(channelPort)}; }
};


struct PeerChannelConfig
{
	channel::ReliabilityConfig reliability{};
	channel::HeartbeatConfig   heartbeat{};

	// Application payload that may wait for its callback
	size_t					   deliveryBacklogLimit{size_t{2} * internal::MaxMessagePayload};
};


class PeerChannel : private ThreadBase
{
public:
	// Remotes a channel keeps state for; packets from further unknown sources are dropped
	static constexpr size_t MaxLinks			= 256;

	// Datagrams drained from the socket before timers and callers get their turn
	static constexpr size_t MaxDatagramsPerPass = 256;

	// applicationQueue: the thread application messages are delivered on. Without one they share the channel's delivery thread.
	explicit PeerChannel(net::DatagramSocketFactory socketFactory = {}, const PeerChannelConfig &config = {}, TaskQueue *applicationQueue = nullptr);
	~PeerChannel() override;
	PeerChannel(const PeerChannel &)			= delete;
	PeerChannel &operator=(const PeerChannel &) = delete;

	bool		 init(const std::string &localComputerName);
	void		 deinit();

	// Applies to links created afterwards: set before init()
	void		 setConfig(const PeerChannelConfig &config);

	// Binds the channel socket to the adapter address. Resets all links.
	void		 setLocalIPv4(const net::IPv4Address &localIPv4);

	// Starts the I/O loop (receiving, retransmissions, heartbeats) and the delivery thread
	void		 start() override;

	// Also wakes flush() and waiting sendMessage() calls: without the loop no acknowledgement can arrive anymore.
	// Messages that were received but not delivered yet are discarded.
	void		 stop() override;

	int			 getBoundPort() const { return mBoundPort.load(); }

	// Set before start(). Invoked on the delivery thread
	void		 setConnectionCallbacks(ChannelConnectionCallbacks cb) { mConnectionCallbacks = std::move(cb); }
	void		 setValidationCallbacks(ChannelValidationCallbacks cb) { mValidationCallbacks = std::move(cb); }
	void		 setOnSocketBound(SocketBoundCallback cb) { mOnSocketBound = std::move(cb); }
	void		 setMessageCallback(ChannelMessageCallback cb) { mMessageCallback = std::move(cb); }
	void		 setOnPeerLost(PeerLostCallback cb) { mOnPeerLost = std::move(cb); }

	// Peer registry (fed by discovery)
	void		 registerPeer(const std::string &displayName, const net::IPv4Address &ipv4, const int channelPort);
	void		 unregisterPeer(const std::string &displayName);

	// Control signals, always reliable
	bool		 sendConnectRequest(const std::string &computerName);
	bool		 sendConnectAnswer(const std::string &computerName, bool requestAccepted, const std::string &reason = {});
	bool		 sendDisconnect(const std::string &computerName);
	bool		 sendReadyFlag(const std::string &computerName, bool ready = true);

	bool		 sendValidationRequest(const std::string &computerName, RemoteRequest request);
	bool		 sendSecretResponse(const std::string &computerName, const std::string &secret);
	bool		 sendVersionResponse(const std::string &computerName, const std::string &version);
	bool		 sendValidationHandshake(const std::string &computerName);

	// Application message. False if the peer is unknown, the message is too large or the send queue refused it.
	// A reliable message waits up to timeout for room in a full send queue (OverflowPolicy::DropNewest) before it is refused.
	bool		 sendMessage(const std::string &computerName, uint32_t type, std::span<const uint8_t> data, DeliveryMode mode, std::chrono::milliseconds timeout = {});

	// Heartbeats and silence detection for the peer of a session
	void		 setKeepAlive(const std::string &computerName, bool enabled);

	// Discards application messages to the peer that were not sent yet
	void		 dropApplicationTraffic(const std::string &computerName);

	// Waits until everything reliable to the peer was acknowledged. Requires the I/O loop to run.
	bool		 flush(const std::string &computerName, std::chrono::milliseconds timeout);

private:
	using Clock		= std::chrono::steady_clock;
	using TimePoint = Clock::time_point;

	struct OutgoingDatagram
	{
		net::SocketAddress		  to;
		channel::OutgoingDatagram datagram;
	};

	struct InboundMessage
	{
		net::SocketAddress		  from;
		channel::DeliveredMessage message;
	};

	// Counts application payload as waiting for its callback for as long as it exists: the task that delivers the
	// payload owns it, whether that task runs or is discarded with its queue
	struct BacklogShare
	{
		BacklogShare(std::shared_ptr<std::atomic<size_t>> backlog, const size_t bytes) : backlog(std::move(backlog)), bytes(bytes) { this->backlog->fetch_add(bytes); }
		~BacklogShare() { backlog->fetch_sub(bytes); }

		BacklogShare(const BacklogShare &)									 = delete;
		BacklogShare						&operator=(const BacklogShare &) = delete;

		std::shared_ptr<std::atomic<size_t>> backlog;
		size_t								 bytes;
	};

	struct LostPeer
	{
		net::SocketAddress address;
		std::string		   reason;
	};

	// Work collected under mLinksMutex and carried out after releasing it
	struct Batch
	{
		std::vector<OutgoingDatagram> datagrams;
		std::vector<InboundMessage>	  messages;
		std::vector<LostPeer>		  lostPeers;
		bool						  wakeIoThread{false}; // a timer became due earlier than the I/O thread is waiting for
	};

	void				   run() override;
	void				   interruptWork() override;
	void				   receivePending(net::IDatagramSocket &socket);
	void				   handleDatagram(const net::SocketAddress &from, std::span<const uint8_t> bytes);
	void				   serviceTimers();
	void				   waitForWork(net::IDatagramSocket &socket);

	// Caller holds mLinksMutex
	channel::ReliableLink *linkFor(const net::SocketAddress &address, bool create);

	// Everything a link produced: datagrams to send, received messages and events. I/O thread only: it alone hands
	// messages over, so they reach their callbacks in the order the link delivered them.
	void				   collect(const net::SocketAddress &address, channel::ReliableLink &link, Batch &batch, TimePoint now);

	// Only the datagrams to send: for threads that queue messages. Their datagrams may reach the wire slightly before
	// or after those of the I/O thread, which the links tolerate (ReliabilityConfig::reorderDelay).
	void				   collectOutgoing(const net::SocketAddress &address, channel::ReliableLink &link, Batch &batch, TimePoint now);

	// Records a timer. True if the I/O thread has to be woken, because it waits for a later one.
	bool				   noteDeadline(std::optional<TimePoint> due);

	// Caller must not hold mLinksMutex
	void				   execute(Batch &batch);
	void				   deliver(Batch &batch);
	void				   routeControl(const net::SocketAddress &from, std::span<const uint8_t> body) const;
	void				   routeApplication(const net::SocketAddress &from, uint32_t type, std::vector<uint8_t> body) const;
	void				   reportLostPeer(const LostPeer &lost) const;

	bool				   sendSignal(const std::string &computerName, SignalType type, decltype(SignalPacket::payload) payload = PayloadEmpty{});
	bool				   queueReliable(const PeerEndpoint &peer, channel::ChannelId channelId, uint32_t tag, std::vector<uint8_t> body, std::chrono::milliseconds timeout = {});

	PeerEndpoint		   resolvePeer(const std::string &computerName) const;
	std::string			   nameOf(const net::SocketAddress &address) const;

	// Caller holds mPeerRegistryMutex
	void				   forgetAddress(const net::SocketAddress &address, const std::string &displayName);

	void				   resetLinks();

	std::shared_ptr<net::IDatagramSocket>								 socket() const;


	net::DatagramSocketFactory											 mSocketFactory;

	mutable std::mutex													 mSocketMutex;
	std::shared_ptr<net::IDatagramSocket>								 mSocket;
	std::string															 mLocalComputerName;
	net::IPv4Address													 mLocalIPv4;
	std::atomic<int>													 mBoundPort{0};

	std::vector<uint8_t>												 mReceiveBuffer; // I/O thread only
	std::vector<net::SocketAddress>										 mTouched;		 // I/O thread only: links that received something in the current pass

	// Received messages and lost peers are handed over to these threads, so callbacks never hold up the I/O loop
	TaskQueue															 mDelivery;
	TaskQueue															*mApplicationQueue; // not owned. Null: application messages go through mDelivery as well

	// Application payload handed over and not delivered yet, in bytes
	std::shared_ptr<std::atomic<size_t>>								 mDeliveryBacklog{std::make_shared<std::atomic<size_t>>(0)};

	std::atomic<bool>													 mInitialized{false};
	ChannelConnectionCallbacks											 mConnectionCallbacks;
	ChannelValidationCallbacks											 mValidationCallbacks;
	SocketBoundCallback													 mOnSocketBound;
	ChannelMessageCallback												 mMessageCallback;
	PeerLostCallback													 mOnPeerLost;

	// Reliability state, guarded by mLinksMutex
	mutable std::mutex													 mLinksMutex;
	std::condition_variable												 mLinksChanged; // signalled after links changed (acknowledgements, resets); flush() waits on it
	PeerChannelConfig													 mConfig;
	std::map<net::SocketAddress, std::unique_ptr<channel::ReliableLink>> mLinks;
	channel::HeartbeatService											 mHeartbeat;
	std::optional<TimePoint>											 mEarliestDeadline; // never later than the next timer of any link or heartbeat
	std::optional<TimePoint>											 mWaitingUntil;		// while the I/O thread waits: when that wait ends

	std::map<std::string, PeerEndpoint>									 mPeerRegistry;		// key = displayName
	std::map<net::SocketAddress, std::string>							 mNameByAddress;	// the same peers by address
	mutable std::mutex													 mPeerRegistryMutex;
};

} // namespace netlink
