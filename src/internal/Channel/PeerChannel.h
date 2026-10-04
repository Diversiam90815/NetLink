/*
  ==============================================================================
	Module:         PeerChannel
	Description:    The dedicated UDP socket all peer-to-peer traffic runs on
  ==============================================================================
*/

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Mailbox.h"
#include "SendScheduler.h"
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

	// Datagrams the channel sends per second, to all peers together. 0 = unlimited.
	uint32_t				   maxSendRate{80'000};
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

	// Starts the I/O loop (receiving, sending, retransmissions, heartbeats) and the delivery thread
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

	struct LoopStats
	{
		uint64_t steps{0};		  // rounds of the I/O loop
		uint64_t overdueWaits{0}; // waits that started with a timer already due
		uint64_t datagramsSent{0};	// what the socket accepted
		uint64_t budgetTicks{0};	// rounds that only waited for the next tick of the send budget
		uint64_t budgetTickTime{0}; // ... and how long they took together, in microseconds
	};

	LoopStats loopStats() const
	{
		return {.steps			= mSteps.load(),
				.overdueWaits	= mOverdueWaits.load(),
				.datagramsSent	= mDatagramsSent.load(),
				.budgetTicks	= mBudgetTicks.load(),
				.budgetTickTime = mBudgetTickTime.load()};
	}

	// Runs one round of the I/O loop on the calling thread and calls the callbacks before it returns. For a channel
	// that was not started and whose owner supplies the time, like a test under a virtual clock. Returns when the next
	// round is due if nothing arrives before.
	std::optional<std::chrono::steady_clock::time_point> poll(std::chrono::steady_clock::time_point now);

private:
	using Clock		= std::chrono::steady_clock;
	using TimePoint = Clock::time_point;

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

	// What one step of the I/O loop hands over to the delivery threads
	struct EventBatch
	{
		std::vector<InboundMessage> messages;
		std::vector<LostPeer>		lostPeers;
	};

	struct LinkState
	{
		explicit LinkState(const channel::ReliabilityConfig &config) : link(config) {}

		channel::ReliableLink link;
		bool				  backlog{false};	// the mailbox may hold messages for this link
		bool				  unsettled{false}; // flush() callers were not told yet that everything it took is acknowledged
		std::optional<TimePoint> deadline;		   // its next timer, as of the last time it was looked at
		uint8_t					 scheduled{0};	   // the send classes it waits in the scheduler for
	};

	enum class Transmit
	{
		Sent,
		Lost,	 // the destination cannot be reached: counts as sent and lost on the way
		Refused, // the socket did not take it: offered again later
	};

	// --- I/O thread ------------------------------------------------------------

	void		 run() override;
	void		 interruptWork() override;
	void		 loop();
	void		 fail();

	// One round of the loop: commands, receiving, timers, sending. The time is passed in: a step never reads the clock.
	EventBatch	 step(net::IDatagramSocket &socket, TimePoint now);
	void		 apply(const channel::Mailbox::PostedCommand &command, TimePoint now);
	void		 receivePending(net::IDatagramSocket &socket, TimePoint now);
	void		 handleDatagram(const net::SocketAddress &from, std::span<const uint8_t> bytes, TimePoint now);
	void		 serviceTimers(EventBatch &batch, TimePoint now);
	void		 feed(const net::SocketAddress &address, LinkState &state);
	void									  collect(const net::SocketAddress &address, LinkState &state, EventBatch &batch);
	void									  sendAcks(const net::SocketAddress &address, LinkState &state, net::IDatagramSocket &socket, TimePoint now);
	void									  schedule(const net::SocketAddress &address, LinkState &state, TimePoint now);
	void									  sendData(net::IDatagramSocket &socket, TimePoint now);
	Transmit								  transmit(net::IDatagramSocket &socket, const net::SocketAddress &address, const channel::OutgoingDatagram &datagram);
	void									  forgetLinks();
	void									  endAllSessions(EventBatch &batch, const char *reason);
	void		 waitForWork(net::IDatagramSocket &socket);
	LinkState	*linkFor(const net::SocketAddress &address, bool create);

	void		 deliver(EventBatch &batch);
	void		 routeControl(const net::SocketAddress &from, std::span<const uint8_t> body) const;
	void		 routeApplication(const net::SocketAddress &from, uint32_t type, std::vector<uint8_t> body) const;
	void		 reportLostPeer(const LostPeer &lost) const;

	// --- Any thread ------------------------------------------------------------

	bool		 sendSignal(const std::string &computerName, SignalType type, decltype(SignalPacket::payload) payload = PayloadEmpty{});
	bool		 push(const PeerEndpoint &peer, channel::Mailbox::Lane lane, uint32_t tag, std::vector<uint8_t> body, std::chrono::milliseconds timeout = {});

	PeerEndpoint resolvePeer(const std::string &computerName) const;
	std::string	 nameOf(const net::SocketAddress &address) const;

	// Caller holds mPeerRegistryMutex
	void		 forgetAddress(const net::SocketAddress &address, const std::string &displayName);

	void		 resetLinks();
	void		 wakeIoThread();

	std::shared_ptr<net::IDatagramSocket>	  socket() const;


	net::DatagramSocketFactory				  mSocketFactory;

	mutable std::mutex						  mSocketMutex;
	std::shared_ptr<net::IDatagramSocket>	  mSocket;
	std::string								  mLocalComputerName;
	net::IPv4Address						  mLocalIPv4;
	PeerChannelConfig						  mPendingConfig; // set by setConfig(), taken over by the I/O thread
	std::atomic<int>						  mBoundPort{0};

	// Received messages and lost peers are handed over to these threads, so callbacks never hold up the I/O loop
	TaskQueue								  mDelivery;
	TaskQueue								 *mApplicationQueue; // not owned. Null: application messages go through mDelivery as well

	// Application payload handed over and not delivered yet, in bytes
	std::shared_ptr<std::atomic<size_t>>	  mDeliveryBacklog{std::make_shared<std::atomic<size_t>>(0)};

	std::atomic<bool>						  mInitialized{false};
	ChannelConnectionCallbacks				  mConnectionCallbacks;
	ChannelValidationCallbacks				  mValidationCallbacks;
	SocketBoundCallback						  mOnSocketBound;
	ChannelMessageCallback					  mMessageCallback;
	PeerLostCallback						  mOnPeerLost;

	// Everything other threads want from the I/O thread goes through here
	channel::Mailbox						  mMailbox;

	// Owned by the I/O thread
	PeerChannelConfig						  mConfig;
	std::map<net::SocketAddress, LinkState>	  mLinks;
	channel::HeartbeatService				  mHeartbeat;
	channel::SendScheduler					  mScheduler;
	std::optional<TimePoint>				  mNextWake; // never later than the next timer of any link or heartbeat
	std::vector<uint8_t>					  mReceiveBuffer;
	std::vector<net::SocketAddress>			  mTouched;	 // links with something to send, deliver or report in the current step
	std::vector<net::SocketAddress>			  mAcksHeldBack;		  // links with acknowledgements the socket did not take yet
	bool									  mSocketBlocked{false};  // in the current step: the socket takes nothing anymore
	bool									  mSocketFailed{false};	  // ... and will not recover by itself
	bool									  mWaitingForTick{false}; // the last step left datagrams waiting for tokens or for the socket
	bool									  mTickTimedOut{false};	  // ... and nothing else ended the wait that followed
	TimePoint								  mStepStartedAt{};
	channel::Mailbox::Work					  mWork;

	std::atomic<uint64_t>					  mSteps{0};
	std::atomic<uint64_t>					  mOverdueWaits{0};
	std::atomic<uint64_t>					  mDatagramsSent{0};
	std::atomic<uint64_t>					  mBudgetTicks{0};
	std::atomic<uint64_t>					  mBudgetTickTime{0};

	std::map<std::string, PeerEndpoint>		  mPeerRegistry;  // key = displayName
	std::map<net::SocketAddress, std::string> mNameByAddress; // the same peers by address
	mutable std::mutex						  mPeerRegistryMutex;
};

} // namespace netlink
