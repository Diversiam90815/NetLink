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
	ReliableOrdered,	 // Delivered exactly once, in send order
	UnreliableSequenced, // May be dropped; stale messages are discarded
};


// Transport used for the data connection once two peers agreed to connect.
enum class TransportKind : uint8_t
{
	Tcp,
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
	std::string	  localDisplayName{};
	int			  discoveryPort{5555};
	std::string	  broadcastAddress{"255.255.255.255"};
	std::string	  secret{"NetLink"};
	TransportKind transport{TransportKind::Tcp};
};


// --- Main Facade ----------------------------------

class NetLink
{
public:
	NetLink();
	~NetLink();

	// Non-copyable, movable
	NetLink(const NetLink &)			= delete;
	NetLink &operator=(const NetLink &) = delete;
	NetLink(NetLink &&) noexcept		= default;
	NetLink					   &operator=(NetLink &&) noexcept;

	// Register all callbacks. Call before init()
	// Callbacks run one at a time on NetLink's event thread, never while internal locks are held:
	// calling back into NetLink from a callback is safe (except destroying the NetLink instance).
	void						configure(const NetLinkConfig &config, const NetLinkCallbacks &callbacks);

	// Initialize networking (adapter enumeration, sockets). Selects the preferred adapter if none is active yet.
	bool						init();

	// Tear down everything (notifies a connected remote first). Safe to call multiple times
	void						shutdown();


	// -- Discovery --------------------------

	// Start broadcasting as a host or searching for hosts
	bool						startDiscovery();

	// Stop active discovery
	void						stopDiscovery();

	// Currently validated, compatible remotes (snapshot)
	std::vector<Endpoint>		getPotentialEndpoints();


	// -- Connection ------------------------------

	// Client: connect to a discovered endpoint
	bool						connectTo(const Endpoint &remote);

	// Accept or reject a pending inbound connection (host side)
	void						respondToConnection(bool accepted);

	// Disconnect active session
	void						disconnect();

	// Current connection state
	ConnectionState				getConnectionState() const;


	// -- Messaging -----------------------------------

	// Send a message to the connected peer
	bool						send(const Message &message, DeliveryMode mode = DeliveryMode::ReliableOrdered);

	// Send a typed message with raw bytes
	bool						send(uint32_t type, const std::vector<uint8_t> &payload, DeliveryMode mode = DeliveryMode::ReliableOrdered);


	// -- Network adapters -------------------------------

	// Available adapters on the system
	std::vector<NetworkAdapter> getAvailableAdapters();

	// Switch active adapter by ID
	bool						setActiveAdapter(const int &adapterID);

	// Get the ID of the currently active adapter (0 if none)
	int							getActiveAdapterID() const;

private:
	struct Impl;
	std::unique_ptr<Impl> pImpl{};
};

} // namespace netlink
