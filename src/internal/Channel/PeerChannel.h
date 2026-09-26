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
#include <span>
#include <string>
#include <vector>

#include "SignalPacket.h"
#include "ThreadBase.h"
#include "Fragmentation/FragmentationService.h"
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
	net::SocketAddress address() const { return {IPv4, static_cast<uint16_t>(channelPort)}; }
};


struct PeerChannelConfig
{
	channel::ReliabilityConfig reliability{};
	channel::HeartbeatConfig   heartbeat{};
};


class PeerChannel : private ThreadBase
{
public:
	// Remotes a channel keeps state for; packets from further unknown sources are dropped
	static constexpr size_t MaxLinks = 256;

	explicit PeerChannel(net::DatagramSocketFactory socketFactory = {}, const PeerChannelConfig &config = {});
	~PeerChannel() override;
	PeerChannel(const PeerChannel &)			= delete;
	PeerChannel &operator=(const PeerChannel &) = delete;

	bool		 init(const std::string &localComputerName);
	void		 deinit();

	// Applies to links created afterwards: set before init()
	void		 setConfig(const PeerChannelConfig &config);

	// Binds the channel socket to the adapter address. Resets all links.
	void		 setLocalIPv4(const net::IPv4Address &localIPv4);

	// I/O loop: receiving, retransmissions and heartbeats
	using ThreadBase::start;
	using ThreadBase::stop;

	int	 getBoundPort() const { return mBoundPort.load(); }

	// Set before start(). Invoked on the channel thread, never while internal locks are held.
	void setConnectionCallbacks(ChannelConnectionCallbacks cb) { mConnectionCallbacks = std::move(cb); }
	void setValidationCallbacks(ChannelValidationCallbacks cb) { mValidationCallbacks = std::move(cb); }
	void setOnSocketBound(SocketBoundCallback cb) { mOnSocketBound = std::move(cb); }
	void setMessageCallback(ChannelMessageCallback cb) { mMessageCallback = std::move(cb); }
	void setOnPeerLost(PeerLostCallback cb) { mOnPeerLost = std::move(cb); }

	// Peer registry (fed by discovery)
	void registerPeer(const std::string &displayName, const net::IPv4Address &ipv4, const int channelPort);
	void unregisterPeer(const std::string &displayName);

	// Control signals, always reliable
	bool sendConnectRequest(const std::string &computerName);
	bool sendConnectAnswer(const std::string &computerName, bool requestAccepted, const std::string &reason = {});
	bool sendDisconnect(const std::string &computerName);
	bool sendReadyFlag(const std::string &computerName, bool ready = true);

	bool sendValidationRequest(const std::string &computerName, RemoteRequest request);
	bool sendSecretResponse(const std::string &computerName, const std::string &secret);
	bool sendVersionResponse(const std::string &computerName, const std::string &version);
	bool sendValidationHandshake(const std::string &computerName);

	// Application message. False if the peer is unknown, the message is too large or the send queue refused it.
	bool sendMessage(const std::string &computerName, uint32_t type, std::span<const uint8_t> data, DeliveryMode mode);

	// Heartbeats and silence detection for the peer of a session
	void setKeepAlive(const std::string &computerName, bool enabled);

	// Discards application messages to the peer that were not sent yet
	void dropApplicationTraffic(const std::string &computerName);

	// Waits until everything reliable to the peer was acknowledged. Requires the I/O loop to run.
	bool flush(const std::string &computerName, std::chrono::milliseconds timeout);

private:
	using Clock		= std::chrono::steady_clock;
	using TimePoint = Clock::time_point;

	struct OutgoingDatagram
	{
		net::SocketAddress	 to;
		std::vector<uint8_t> bytes;
	};

	struct InboundMessage
	{
		net::SocketAddress			from;
		channel::ReassembledMessage message;
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
	};

	void								  run() override;
	void								  receiveDatagram();
	void								  handleDatagram(const net::SocketAddress &from, std::span<const uint8_t> bytes);
	void								  serviceTimers();
	std::chrono::milliseconds			  nextWait();

	// Caller holds mLinksMutex
	channel::ReliableLink				 *linkFor(const net::SocketAddress &address, bool create);
	void								  collect(const net::SocketAddress &address, channel::ReliableLink &link, Batch &batch, TimePoint now);

	// Caller must not hold mLinksMutex
	void								  execute(Batch &batch);
	void								  routeControl(const net::SocketAddress &from, std::span<const uint8_t> body);
	void								  routeApplication(const net::SocketAddress &from, std::span<const uint8_t> body);

	bool								  sendSignal(const std::string &computerName, SignalType type, decltype(SignalPacket::payload) payload = PayloadEmpty{});
	bool								  queueReliable(const PeerEndpoint &peer, channel::ChannelId channelId, std::vector<uint8_t> body);

	PeerEndpoint						  resolvePeer(const std::string &computerName) const;
	std::string							  nameOf(const net::SocketAddress &address) const;

	void								  resetLinks();

	std::shared_ptr<net::IDatagramSocket> socket() const;


	net::DatagramSocketFactory			  mSocketFactory;

	mutable std::mutex					  mSocketMutex;
	std::shared_ptr<net::IDatagramSocket> mSocket;
	std::string							  mLocalComputerName;
	net::IPv4Address					  mLocalIPv4;
	std::atomic<int>					  mBoundPort{0};

	std::vector<uint8_t>				  mReceiveBuffer; // channel thread only

	std::atomic<bool>					  mInitialized{false};
	ChannelConnectionCallbacks			  mConnectionCallbacks;
	ChannelValidationCallbacks			  mValidationCallbacks;
	SocketBoundCallback					  mOnSocketBound;
	ChannelMessageCallback				  mMessageCallback;
	PeerLostCallback					  mOnPeerLost;

	// Reliability state, guarded by mLinksMutex
	mutable std::mutex					  mLinksMutex;
	PeerChannelConfig					  mConfig;
	std::map<net::SocketAddress, std::unique_ptr<channel::ReliableLink>> mLinks;
	channel::FragmentationService										 mFragmentation;
	channel::HeartbeatService											 mHeartbeat;

	std::map<std::string, PeerEndpoint>									 mPeerRegistry; // key = displayName
	mutable std::mutex													 mPeerRegistryMutex;
};

} // namespace netlink
