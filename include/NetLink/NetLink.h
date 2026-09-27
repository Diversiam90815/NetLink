/*
==============================================================================
	Module:         NetLink
	Description:    API for NetLink library
  ==============================================================================
*/

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>


namespace netlink
{

// --- Public types ---------------------------------------

struct Endpoint
{
	std::string IPAddress{};
	int			port{0};
	std::string displayName{};

	bool		operator==(const Endpoint &other) const { return IPAddress == other.IPAddress && port == other.port; }
	bool		isValid() const { return !IPAddress.empty() && port != 0; }
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
	int				id{};
	AdapterPriority priority{AdapterPriority::Suppressed};

	bool			isValid() const { return !adapterName.empty() && !networkName.empty() && !ipv4.empty() && id != 0; }
};


// Opaque message envelope
struct Message
{
	uint32_t			 type{0};
	std::vector<uint8_t> data{};
};


// Delivery guarantee requested for a message.
enum class DeliveryMode : uint8_t
{
	ReliableOrdered,	 // Acknowledged and retransmitted until it arrives. Up to 16 MiB, larger messages are fragmented.
	UnreliableSequenced, // Without acknowledgement: message may be dropped. Must fit into a single datagram (about 1.1 KB)
};


// What happens when the queue of reliable messages waiting to be sent is full.
enum class OverflowPolicy : uint8_t
{
	DropNewest, // The new message is refused: send() returns false (backpressure)
	DropOldest, // The oldest message that was not sent yet is discarded to make room
};


enum class ConnectionState
{
	None,
	Hosting,
	Searching,
	PendingInbound,
	Connected,
	Disconnected,
	Error,
};


struct ConnectionEvent
{
	ConnectionState state{ConnectionState::None};
	std::string		errorMessage{};
	Endpoint		remote{};
};


// --- Callbacks ------------------------------------------

struct NetLinkCallbacks
{
	// A compatible remote was discovered and validated (matching secret): connectTo() can be called
	std::function<void(const Endpoint &remote)>		   onRemoteDiscovered;

	// A previously discovered remote stopped announcing and is no longer reachable.
	// Not raised for the peer of an active connection, which may legitimately go quiet.
	std::function<void(const Endpoint &remote)>		   onRemoteLost;

	// Connected state changed (connected, disconnected, error, etc.)
	std::function<void(const ConnectionEvent)>		   onConnectionChanged;

	// An inbound message was received from the remote peer
	std::function<void(const Message &message)>		   onMessageReceived;

	// The active network adapter changed (selected automatically in init() or via setActiveAdapter())
	std::function<void(const NetworkAdapter &adapter)> onNetworkAdapterChanged;
};


// --- Configuration ---------------------------------------

struct NetLinkConfig
{
	std::string	   localDisplayName{};
	int			   discoveryPort{5555};
	std::string	   broadcastAddress{"255.255.255.255"};
	std::string	   secret{"NetLink"};
	std::string	   applicationVersion{}; // Two peers are compatible when the major and minor components match; patch and build number are ignored

	// Reliable messages that may wait for room in the send window, and what happens once that many are waiting
	size_t		   sendQueueCapacity{1024};
	OverflowPolicy sendQueueOverflow{OverflowPolicy::DropNewest};
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

	// Register all callbacks. Call before init()
	// Callbacks run one at a time on NetLink's event thread, never while internal locks are held:
	// calling back into NetLink from a callback is safe (except destroying the NetLink instance).
	void						configure(const NetLinkConfig &config, const NetLinkCallbacks &callbacks) const;

	// Initialize networking (adapter enumeration, sockets). Selects the preferred adapter if none is active yet.
	bool						init() const;

	// Tear down everything (notifies a connected remote first). Safe to call multiple times
	void						shutdown() const;


	// -- Discovery --------------------------

	// Start broadcasting as a host or searching for hosts
	bool						startDiscovery() const;

	// Stop active discovery
	void						stopDiscovery() const;

	// Currently validated, compatible remotes (snapshot)
	std::vector<Endpoint>		getPotentialEndpoints() const;


	// -- Connection ------------------------------

	// Client: connect to a discovered endpoint
	bool						connectTo(const Endpoint &remote) const;

	// Accept or reject a pending inbound connection (host side)
	void						respondToConnection(bool accepted) const;

	// Disconnect active session
	void						disconnect() const;

	// Current connection state
	ConnectionState				getConnectionState() const;


	// -- Messaging -----------------------------------

	// Send a message to the connected peer. Returns false when not connected, when an unreliable message does not fit into
	// one datagram, or when the send queue is full under OverflowPolicy::DropNewest.
	bool						send(const Message &message, DeliveryMode mode = DeliveryMode::ReliableOrdered) const;

	// Send a typed message with raw bytes
	bool						send(uint32_t type, const std::vector<uint8_t> &payload, DeliveryMode mode = DeliveryMode::ReliableOrdered) const;


	// -- Network adapters -------------------------------

	// Available adapters on the system
	std::vector<NetworkAdapter> getAvailableAdapters() const;

	// Switch active adapter by ID
	bool						setActiveAdapter(const int &adapterID) const;

	// Get the ID of the currently active adapter (0 if none)
	int							getActiveAdapterID() const;

private:
	struct Impl;
	std::unique_ptr<Impl> pImpl{};
};

} // namespace netlink
