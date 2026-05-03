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


struct NetworkAdapter
{
	std::string adapterName{};
	std::string networkName{};
	std::string ipv4{};
	std::string subnet{};
	int			id{};
	bool		isDefaultRoute{false};

	bool		isValid() const { return !adapterName.empty() && !networkName.empty() && !ipv4.empty() && id != 0; }
};

// Opaque message envelope
struct Message
{
	uint32_t			 type{0};
	std::vector<uint8_t> data{};
};

enum class ConnectionState
{
	None,
	Hosting,
	Searching,
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
	// A remote endpoint was discovered
	std::function<void(const Endpoint &remote)>		   onRemoteDiscovered;

	// Connected state changed (connected, disconnected, error, etc.)
	std::function<void(const ConnectionEvent)>		   onConnectionChanged;

	// An inbound message was received from the remote peer
	std::function<void(const Message &message)>		   onMessageReceived;

	// A network adapter change was detected
	std::function<void(const NetworkAdapter &adapter)> onNetworkAdapterChanged;
};


// --- Configuration ---------------------------------------

struct NetLinkConfig
{
	std::string	   localDisplayName{};
	std::string	   localIPv4{};
	unsigned short tcpPort{0};
	int			   discoveryPort{5555};
	std::string	   broadcastAddress{"255.255.255.255"};
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
	NetLink(NetLink &&) noexcept;
	NetLink					   &operator=(NetLink &&) noexcept;

	// Register all callbacks. Call before init()
	void						configure(const NetLinkConfig &config, const NetLinkCallbacks &callbacks);

	// Initialize networking (socket, adapter enumeration,..)
	bool						init();

	// Tear down everything. Safe to call multiple times
	void						shutdown();


	// -- Discovery --------------------------

	// Start broadcasting as a host or searching for hosts
	bool						startDiscovery();

	// Stop active discovery
	void						stopDiscovery();

	// Currently discovered remotes (snapshot)
	std::vector<Endpoint>		getDiscoveredEndpoints();


	// -- Connection ------------------------------

	// Host: begin accepting incoming connections
	bool						hostSession();

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
	bool						send(const Message &message);

	// Send a typed message with raw bytes
	bool						send(uint32_t type, const std::vector<uint8_t> &payload);


	// -- Network adapters -------------------------------

	// Available adapters on the system
	std::vector<NetworkAdapter> getAvailableAdapters();

	// Switch active adapter by ID
	bool						setActiveAdapter(const int &adapterID);

private:
	struct Impl;
	std::unique_ptr<Impl> pImpl{};
};

} // namespace netlink
