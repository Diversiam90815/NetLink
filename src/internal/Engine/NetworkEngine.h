/*
  ==============================================================================
	Module:         NetworkEngine
	Description:    Everything that happens on the network: discovery, sessions
					and their links, on one UDP socket and one thread
  ==============================================================================
*/

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <vector>

#include "NetLink/NetLink.h"

#include "Channel/Mailbox.h"
#include "Channel/Reliability/ReliableLink.h"
#include "Channel/SendScheduler.h"
#include "Discovery/DiscoveryLogic.h"
#include "Socket/IDatagramSocket.h"
#include "TransportConstants.h"


namespace netlink
{

// The address of the network adapter the engine runs on
struct LocalInterface
{
	net::IPv4Address ip;
	net::IPv4Address mask; // none: discovery is not limited to a subnet

	bool			 operator==(const LocalInterface &) const = default;
};

// Asked for the adapter's address as it is right now. None: the adapter has none.
using LocalInterfaceProvider = std::function<std::optional<LocalInterface>()>;


struct EngineConfig
{
	std::string			 displayName;
	std::string			 appId;
	std::string			 appVersion;
	uint16_t			 discoveryPort{5555};

	// Bytes of messages that may wait to be sent, per acknowledged lane of one peer
	size_t				 sendQueueBytes{channel::DefaultSendQueueBytes};

	// Datagrams the engine sends per second, to all peers together. 0 = unlimited.
	uint32_t			 maxSendRate{channel::DefaultMaxSendRate};

	channel::LinkTimings timings{};

	// Connection requests are accepted without asking the application
	bool				 autoAccept{true};

	// 0: drawn at random
	uint64_t			 instanceId{0};
};


// Media messages of one peer the application has not taken yet: only the newest ones are kept
struct MediaInbox
{
	std::mutex			mutex;
	std::deque<Message> waiting;
};


struct EngineEvent
{
	enum class Kind : uint8_t
	{
		PeerDiscovered,
		PeerLost,
		ConnectionRequest,
		Connected,
		Disconnected,
		Message,
		AdapterChanged, // info.address: the address the engine is bound to now
	};

	Kind						kind{Kind::Message};
	PeerId						peer{};
	PeerInfo					info{};
	DisconnectReason			reason{DisconnectReason::Local};
	Lane						lane{Lane::Reliable};
	Message						message{};
	std::shared_ptr<MediaInbox> media{}; // Message on Lane::Media: where the message waits

	// The message of a Message event. None for a Media message that newer ones replaced in the meantime.
	std::optional<Message>		takeMessage();
};


// What one step of the engine hands over to the event thread, in the order it happened
struct EventBatch
{
	std::vector<EngineEvent> events;
	std::shared_ptr<void>	 backlog; // counts the payload as waiting for the application for as long as the batch exists
};


class NetworkEngine
{
public:
	using Clock									= std::chrono::steady_clock;
	using TimePoint								= Clock::time_point;

	// Datagrams drained from the socket before timers and callers get their turn
	static constexpr size_t MaxDatagramsPerPass = 256;

	NetworkEngine(EngineConfig config, net::DatagramSocketFactory socketFactory, LocalInterfaceProvider localInterface);
	~NetworkEngine();

	NetworkEngine(const NetworkEngine &)					  = delete;
	NetworkEngine			&operator=(const NetworkEngine &) = delete;


	// --- Any thread ------------------------------------------------------------

	PeerId					 id() const { return mId; }

	void					 setAnnouncing(bool announcing);

	// The adapter may have changed: the engine asks for its address again
	void					 checkInterface();

	bool					 connect(PeerId peer);
	void					 accept(PeerId peer);
	void					 decline(PeerId peer);
	void					 disconnect(PeerId peer);

	SendResult				 send(PeerId peer, uint32_t type, std::vector<uint8_t> &&data, Lane lane, std::chrono::milliseconds timeout = {});
	size_t					 broadcast(uint32_t type, std::span<const uint8_t> data, Lane lane);

	// Waits until everything on Reliable and Bulk to the peer was acknowledged
	bool					 flush(PeerId peer, std::chrono::milliseconds timeout);

	std::vector<PeerInfo>	 peers() const { return mMailbox.discovered(); }
	std::vector<PeerId>		 connectedPeers() const { return mMailbox.connected(); }
	std::optional<PeerStats> stats(PeerId peer) const { return mMailbox.stats(peer); }

	// Ends every session and tells the peers. The engine has finished() once they know, or shortly after.
	void					 shutdown();

	// False after shutdown() and after the loop died
	bool					 isRunning() const { return mMailbox.isRunning(); }

	// Where the engine can be reached, port 0 while it is not bound
	net::SocketAddress		 localEndpoint() const;

	struct LoopStats
	{
		uint64_t steps{0};			// rounds of the I/O loop
		uint64_t overdueWaits{0};	// waits that started with a timer already due
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


	// --- I/O thread ------------------------------------------------------------

	// The loop: steps, hands every batch of events to deliver and waits for work. Returns once the engine has finished
	// or stop was requested.
	void					 run(const std::stop_token &stop, const std::function<void(EventBatch &&)> &deliver);

	// One round of the loop: commands, receiving, timers, sending. The time is passed in: a step never reads the clock.
	EventBatch				 step(TimePoint now);

	// When the next step is due if nothing arrives before
	std::optional<TimePoint> nextWake() const { return mNextWake; }

	bool					 finished() const { return mShutdown && mPeers.empty(); }

private:
	using WireLane = channel::Lane;

	// Application payload that was handed over and waits for its callback
	struct DeliveryBacklog
	{
		std::atomic<size_t>	  bytes{0};
		std::atomic<bool>	  paused{false}; // the senders were asked to hold back for it

		std::mutex			  mutex;
		std::function<void()> onDrained;	 // a paused engine has room again. Gone with the engine.
	};

	struct BacklogShare
	{
		BacklogShare(std::shared_ptr<DeliveryBacklog> backlog, size_t bytes);
		~BacklogShare();

		BacklogShare(const BacklogShare &)								 = delete;
		BacklogShare					&operator=(const BacklogShare &) = delete;

		std::shared_ptr<DeliveryBacklog> backlog;
		size_t							 bytes;
	};

	struct Peer
	{
		enum class State : uint8_t
		{
			Connecting,		  // the Hello, or the Accept, is on its way
			AwaitingDecision, // the application was asked
			Connected,
			Closing,		  // sending what is left, then the goodbye
		};

		Peer(PeerInfo info, const net::SocketAddress &address, const channel::LinkTimings &timings, const uint32_t streamID, channel::AssemblyBudget *budget)
			: info(std::move(info)), address(address), link(timings, streamID, budget)
		{
		}

		PeerInfo							 info;
		net::SocketAddress					 address;
		State								 state{State::Connecting};
		channel::ReliableLink				 link;
		std::deque<channel::OutboundMessage> control;		   // session messages the link has not taken yet
		std::optional<TimePoint>			 deadline;		   // the link's next timer, as of the last time it was looked at
		std::optional<TimePoint>			 stateDeadline;	   // when the current state gives up waiting
		bool								 initiator{false};
		bool								 acceptSent{false};
		bool								 closeSent{false}; // the goodbye is on its way: the link only waits for its acknowledgement
		bool								 announced{false}; // the application knows about the session
		bool								 reported{false};  // ... and was told how it ended
		bool								 ended{false};	   // gone at the end of the step
		bool								 unsettled{false}; // flush() callers were not told yet that everything is acknowledged
		uint8_t								 scheduled{0};	   // the lanes it waits in the scheduler for
		std::shared_ptr<MediaInbox>			 media;
		uint64_t							 mediaSentAtReport{0};
		uint64_t							 mediaReceivedAtReport{0};
		float								 mediaLoss{0.0f};
	};

	class PeerSource;

	enum class Transmit
	{
		Sent,
		Lost,	 // the destination cannot be reached: counts as sent and lost on the way
		Refused, // the socket did not take it: offered again later
	};

	// --- I/O thread ------------------------------------------------------------

	void								  apply(const channel::Mailbox::PostedCommand &command, TimePoint now);
	void								  startSession(PeerId id, TimePoint now);
	void								  endSession(Peer &peer, TimePoint now);
	void								  beginShutdown(TimePoint now);

	// The adapter: binds to it, and ends every session when its address changed or the socket broke
	void								  updateInterface(bool socketFailed);
	void								  bind(const LocalInterface &iface);
	void								  closeSockets();
	void								  endAllSessions(DisconnectReason reason);

	void								  receivePending(TimePoint now);
	void								  handleDatagram(const net::SocketAddress &from, std::span<const uint8_t> bytes, TimePoint now);
	void								  onBeacon(const net::SocketAddress &from, std::span<const uint8_t> bytes, TimePoint now);
	Peer								 *onHello(const net::SocketAddress &from, const channel::DecodedPacket &packet, TimePoint now);
	void								  serviceTimers(TimePoint now);
	void								  tick(TimePoint now);
	void								  sendBeacon(const net::SocketAddress &to, bool reply);
	void								  updatePause();

	// What the link of a peer received and what that means for its session
	void								  advance(Peer &peer, TimePoint now);
	void								  onControl(Peer &peer, const channel::DeliveredMessage &message);
	void								  setConnected(Peer &peer);
	void								  sendAccept(Peer &peer);
	void								  sendDecline(Peer &peer, TimePoint now);
	void								  sendClose(Peer &peer, TimePoint now);
	void								  report(Peer &peer, DisconnectReason reason);
	void								  end(Peer &peer);
	void								  erase(PeerId id);
	bool								  transfersData(const Peer &peer) const;
	PeerStats							  statsOf(Peer &peer) const;

	void								  sendAcks(Peer &peer);
	void								  schedule(Peer &peer, TimePoint now);
	void								  sendData(TimePoint now);
	Transmit							  transmit(const net::SocketAddress &address, const channel::OutgoingDatagram &datagram);

	Peer								 *find(PeerId id);
	Peer								 &addPeer(PeerInfo info, const net::SocketAddress &address, TimePoint now);
	void								  touch(const PeerId id) { mTouched.push_back(id); }
	void								  emit(EngineEvent event) { mEvents.push_back(std::move(event)); }
	EventBatch							  takeBatch();
	void								  waitForWork();
	void								  fail(const std::function<void(EventBatch &&)> &deliver);

	// --- Any thread ------------------------------------------------------------

	void								  wake();


	const EngineConfig					  mConfig;
	const PeerId						  mId;
	net::DatagramSocketFactory			  mSocketFactory;
	LocalInterfaceProvider				  mLocalInterface;

	mutable std::mutex					  mSocketMutex;
	std::shared_ptr<net::IDatagramSocket> mSocket;
	std::condition_variable				  mWakeUp;		 // what the loop waits on while it has no socket
	bool								  mWoken{false}; // work arrived since the last step

	// Application payload handed over and not delivered yet, in bytes
	std::shared_ptr<DeliveryBacklog>	  mDeliveryBacklog{std::make_shared<DeliveryBacklog>()};

	// Everything other threads want from the I/O thread goes through here
	channel::Mailbox					  mMailbox;

	// Owned by the I/O thread
	net::IDatagramSocket				 *mIoSocket{nullptr}; // mSocket, without the lock: only this thread replaces it
	std::unique_ptr<net::IDatagramSocket> mBeaconSocket;
	std::optional<LocalInterface>		  mInterface;
	discovery::DiscoveryLogic			  mDiscovery;
	std::map<PeerId, Peer>				  mPeers;
	std::unordered_map<uint32_t, PeerId>  mByStream;	  // the peers by the stream ID of their link on this side
	channel::SendScheduler				  mScheduler;
	channel::AssemblyBudget				  mAssemblyBudget;
	std::vector<EngineEvent>			  mEvents;		  // of the current step
	bool								  mPaused{false}; // too much waits for the application: senders are asked to hold back
	bool								  mShutdown{false};
	bool								  mCheckInterface{true};
	bool								  mBeaconDue{false};
	bool								  mPeersChanged{false};	  // the discovered peers are published again
	std::optional<TimePoint>			  mNextTick;			  // announcing, expiry of peers and a look at the adapter
	std::optional<TimePoint>			  mNextWake;
	std::vector<uint8_t>				  mReceiveBuffer;
	std::vector<PeerId>					  mTouched;				  // peers with something to send, deliver or report in the current step
	std::vector<PeerId>					  mAcksHeldBack;		  // peers with acknowledgements the socket did not take yet
	std::vector<PeerId>					  mEnded;
	bool								  mSocketBlocked{false};  // in the current step: the socket takes nothing anymore
	bool								  mSocketFailed{false};	  // ... and will not recover by itself
	bool								  mWaitingForTick{false}; // the last step left datagrams waiting for tokens or for the socket
	bool								  mTickTimedOut{false};	  // ... and nothing else ended the wait that followed
	TimePoint							  mStepStartedAt{};
	channel::Mailbox::Work				  mWork;

	std::atomic<uint64_t>				  mSteps{0};
	std::atomic<uint64_t>				  mOverdueWaits{0};
	std::atomic<uint64_t>				  mDatagramsSent{0};
	std::atomic<uint64_t>				  mBudgetTicks{0};
	std::atomic<uint64_t>				  mBudgetTickTime{0};
};

} // namespace netlink
