/*
==============================================================================
	Module:         NetLink
	Description:    API for NetLink library
  ==============================================================================
*/

#pragma once

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>


namespace netlink
{

// --- Limits ---------------------------------------------

inline constexpr size_t MaxMessageSize		= 16u << 20; // Lane::Reliable and Lane::Bulk
inline constexpr size_t MaxMediaMessageSize = 64u << 10; // Lane::Media
inline constexpr size_t MaxDisplayName		= 64;
inline constexpr size_t MaxPeers			= 256;		 // sessions at once


// --- Public types ---------------------------------------

// One run of NetLink on some machine. A peer that is started again is a new peer with a new PeerId.
struct PeerId
{
	uint64_t value{0};

	auto	 operator<=>(const PeerId &) const = default;
};


struct PeerInfo
{
	PeerId		id{};
	std::string displayName{};
	std::string address{};
	uint16_t	port{0};
	std::string appVersion{}; // major.minor
};


// How a message travels. Every lane keeps the order of its own messages; a message on one lane never waits for one on another.
enum class Lane : uint8_t
{
	Reliable, // Acknowledged and sent again until it arrives. Up to 16 MiB.
	Bulk,	  // Like Reliable, but sent last: for large transfers that must not hold up anything else.
	Media,	  // Sent once and never acknowledged: may be lost or overtaken. Up to 64 KiB.
};


enum class SendResult : uint8_t
{
	Queued,
	QueueFull,	  // sendQueueBytes of that lane are waiting already
	TooLarge,
	NotConnected, // no session with that peer
	NotRunning,
};


enum class DisconnectReason : uint8_t
{
	Local,		  // disconnect() or decline() on this side, or the network adapter changed
	Remote,		  // the peer ended the session
	Declined,	  // the peer declined the connection
	Incompatible, // the peer cannot talk to this build
	Lost,		  // the peer does not answer anymore
	NetworkError, // the network is not usable anymore
	Shutdown,	  // stop()
};


struct PeerStats
{
	std::chrono::microseconds rtt{0};
	uint64_t				  bytesQueued{0};	  // waiting to be sent, all lanes
	uint64_t				  bytesSent{0};
	uint64_t				  bytesReceived{0};
	uint64_t				  retransmissions{0}; // datagrams sent again
	uint64_t				  mediaSent{0};		  // Media datagrams
	uint64_t				  mediaDropped{0};	  // Media messages that were replaced by newer ones before they were sent
	uint64_t				  mediaIncomplete{0}; // Media messages of the peer that arrived only in part
	float					  mediaLoss{0.0f};	  // share of the Media datagrams that did not arrive, as the peer last reported
};


enum class AdapterPriority
{
	Suppressed = 1, // Do not show (loopback, down)
	Available  = 2, // Show but not highlighted
	Preferred  = 3	// Highlight as best choice
};


struct NetworkAdapter
{
	std::string		adapterName{};
	std::string		networkName{};
	std::string		ipv4{};
	uint64_t		id{};
	AdapterPriority priority{AdapterPriority::Suppressed};

	bool			isValid() const { return !adapterName.empty() && !networkName.empty() && !ipv4.empty() && id != 0; }
};


enum class LogLevel : uint8_t
{
	Debug,
	Info,
	Warning,
	Error,
};


// Opaque message envelope
struct Message
{
	uint32_t			 type{0};
	std::vector<uint8_t> data{};
};


// --- Callbacks ------------------------------------------

// All callbacks run one at a time on NetLink's event thread, never while internal locks are held: calling back into
// NetLink from a callback is safe (except destroying the NetLink instance).
// For every peer: onConnected comes before its first onMessage, and nothing follows its onDisconnected.
struct NetLinkCallbacks
{
	// A peer running the same application (appId and version) announced itself: connect() can be called
	std::function<void(const PeerInfo &peer)>		   onPeerDiscovered;

	// A discovered peer stopped announcing. Not raised while a session with it exists.
	std::function<void(PeerId peer)>				   onPeerLost;

	// A peer wants to connect: answer with accept() or decline(). Unset: every request is accepted.
	std::function<void(const PeerInfo &peer)>		   onConnectionRequest;

	std::function<void(const PeerInfo &peer)>		   onConnected;

	// How a session ended. Also the outcome of a connect() or a connection request that did not lead to one.
	std::function<void(PeerId peer, DisconnectReason)> onDisconnected;

	std::function<void(PeerId from, Lane, Message &&)> onMessage;

	// The active network adapter changed (selected automatically in start(), via setActiveAdapter(), or its address changed)
	std::function<void(const NetworkAdapter &adapter)> onNetworkAdapterChanged;

	// What NetLink has to say about itself, for the application's own log. Unset: nothing is logged.
	std::function<void(LogLevel, std::string_view message)> onLog;
};


// --- Configuration ---------------------------------------

struct NetLinkConfig
{
	std::string				  displayName{};			 // a label, need not be unique
	std::string				  appId{};					 // required. Only peers with the same appId see each other. Not a secret.
	std::string				  appVersion{};				 // peers are compatible when major and minor match. Empty: the version of the library.
	uint16_t				  discoveryPort{5555};

	size_t					  sendQueueBytes{64u << 20}; // messages that may wait to be sent, per lane of one peer
	uint32_t				  maxSendRate{80'000};		 // datagrams per second, to all peers together. 0 = unlimited.
	std::chrono::milliseconds peerTimeout{5000};		 // a peer that does not answer for this long is lost
};


// --- Main Facade ----------------------------------

class NetLink
{
public:
	NetLink();
	~NetLink();

	// Non-copyable and non-movable
	NetLink(const NetLink &)						  = delete;
	NetLink &operator=(const NetLink &)				  = delete;
	NetLink(NetLink &&)								  = delete;
	NetLink					   &operator=(NetLink &&) = delete;

	// Starts networking on the preferred adapter (or the one chosen before). False without an appId, or when already running.
	bool						start(const NetLinkConfig &config, const NetLinkCallbacks &callbacks);

	// Ends every session (the peers are told) with a final onDisconnected(Shutdown). Called from a callback it returns
	// at once and the events follow. Safe to call multiple times.
	void						stop();


	// -- Discovery --------------------------

	// Announces this peer on the network. Peers that announce themselves are discovered either way.
	bool						startDiscovery();
	void						stopDiscovery();

	// The peers that are discovered right now (snapshot)
	std::vector<PeerInfo>		peers() const;


	// -- Sessions ------------------------------

	// Asks a discovered peer for a session: onConnected or onDisconnected follows. For a peer that asked itself, this
	// is accept(). False if the peer is not discovered, or a session with it exists or is still being ended.
	bool						connect(PeerId peer);

	// The answer to onConnectionRequest
	void						accept(PeerId peer);
	void						decline(PeerId peer);

	// Sends what is still waiting on Lane::Reliable and Lane::Bulk (for up to a second), then ends the session
	void						disconnect(PeerId peer);

	std::vector<PeerId>			connectedPeers() const;


	// -- Messaging -----------------------------------

	// timeout: how long the message may wait for room in a full send queue. With the default of zero send() never blocks.
	SendResult					send(PeerId peer, uint32_t type, std::span<const uint8_t> data, Lane lane = Lane::Reliable, std::chrono::milliseconds timeout = {});
	SendResult					send(PeerId peer, uint32_t type, std::vector<uint8_t> &&data, Lane lane = Lane::Reliable, std::chrono::milliseconds timeout = {});

	// To every connected peer. Returns how many of them took the message.
	size_t						broadcast(uint32_t type, std::span<const uint8_t> data, Lane lane = Lane::Reliable);

	std::optional<PeerStats>	stats(PeerId peer) const;


	// -- Network adapters -------------------------------

	std::vector<NetworkAdapter> getAvailableAdapters() const;

	// Moves all networking to that adapter: every session ends with DisconnectReason::Local
	bool						setActiveAdapter(uint64_t adapterID);

	// 0 if none
	uint64_t					getActiveAdapterID() const;

private:
	struct Impl;
	std::unique_ptr<Impl> pImpl{};
};

} // namespace netlink


template <>
struct std::hash<netlink::PeerId>
{
	size_t operator()(const netlink::PeerId &peer) const noexcept { return std::hash<uint64_t>{}(peer.value); }
};
