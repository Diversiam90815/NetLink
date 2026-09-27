/*
  ==============================================================================
	Module:         NetLinkCore
	Description:    Composition root of the library: owns every service, wires
					them together and delivers public events on a dedicated
					event thread.
  ==============================================================================
*/

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "NetLink/NetLink.h"
#include "Channel/PeerChannel.h"
#include "ConnectionService/ConnectionService.h"
#include "Discovery/DiscoveryService.h"
#include "PeerValidation/PeerValidationService.h"
#include "Socket/IDatagramSocket.h"
#include "Util/TaskQueue.h"


namespace netlink
{

struct NetLinkCoreDependencies
{
	net::DatagramSocketFactory datagramSocketFactory{}; // empty = real UDP sockets
	PeerChannelConfig		   channelConfig{};			// timings of the reliable channel
};


class NetLinkCore
{
public:
	// How long shutdown() waits for the remote to acknowledge the Disconnect
	static constexpr std::chrono::milliseconds ShutdownFlushTimeout{250};

	using Event = std::function<void(const NetLinkCallbacks &callbacks)>;

	explicit NetLinkCore(const NetLinkCoreDependencies &dependencies = {});
	~NetLinkCore();

	NetLinkCore(const NetLinkCore &)					 = delete;
	NetLinkCore			 &operator=(const NetLinkCore &) = delete;

	// Call before init()
	void				  configure(const NetLinkConfig &config, const NetLinkCallbacks &callbacks);

	bool				  init();
	void				  shutdown();

	// Local interface address all networking runs on (the selected network adapter)
	void				  setLocalAddress(const std::string &ipv4, const std::string &subnetMask = {});

	bool				  startDiscovery();
	void				  stopDiscovery();

	std::vector<Endpoint> getPotentialEndpoints() const;

	bool				  connectTo(const Endpoint &remote);
	void				  respondToConnection(bool accepted);
	void				  disconnect();

	ConnectionState		  getConnectionState() const { return mState.load(); }

	bool				  send(uint32_t type, const std::vector<uint8_t> &payload, DeliveryMode mode);

	// Queues a public callback invocation onto the event thread
	void				  postEvent(Event event);

private:
	void									wireServices();

	void									applyLocalAddress();
	void									updateDiscoveryConfig();

	void									onConnectionStatus(const ConnectionStatusUpdate &update);
	void									onValidationResult(const ValidationResult &result);

	void									emitConnectionChanged(ConnectionState state, const std::string &message, const DiscoveryEndpoint &remote);
	void									endSessionTraffic(const std::string &remote);

	static Endpoint							toPublicEndpoint(const DiscoveryEndpoint &endpoint);


	mutable std::mutex						mCallbacksMutex;
	std::shared_ptr<const NetLinkCallbacks> mCallbacks{std::make_shared<const NetLinkCallbacks>()};

	mutable std::mutex						mConfigMutex;
	NetLinkConfig							mConfig;
	net::IPv4Address						mLocalAddress;
	net::IPv4Address						mSubnetMask;
	std::string								mLocalVersion;
	std::atomic<bool>						mInitialized{false};
	std::atomic<ConnectionState>			mState{ConnectionState::None};

	TaskQueue								mEvents;

	// Services
	DiscoveryService						mDiscovery;
	PeerChannel								mChannel;
	PeerChannelConfig						mChannelConfig;
	PeerValidationService					mValidation;
	ConnectionService						mConnectionService;
};

} // namespace netlink
