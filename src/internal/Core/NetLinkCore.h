/*
  ==============================================================================
	Module:         NetLinkCore
	Description:    Composition root of the library: owns every service, wires
					them together and delivers public events on a dedicated
					event thread. Independent of network adapter handling, so
					it can run against injected sockets in tests.
  ==============================================================================
*/

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "NetLink/NetLink.h"
#include "ConnectionService/ConnectionService.h"
#include "Discovery/DiscoveryService.h"
#include "Messaging/RemoteCommunication.h"
#include "PeerValidation/PeerValidationService.h"
#include "Signaling/SignalingService.h"
#include "Socket/IDatagramSocket.h"
#include "Transport/TransportFactory.h"
#include "Util/TaskQueue.h"


namespace netlink
{

struct NetLinkCoreDependencies
{
	net::DatagramSocketFactory datagramSocketFactory{}; // empty = real UDP sockets
};


class NetLinkCore
{
public:
	using Event = std::function<void(const NetLinkCallbacks &callbacks)>;

	explicit NetLinkCore(NetLinkCoreDependencies dependencies = {});
	~NetLinkCore();

	NetLinkCore(const NetLinkCore &)			= delete;
	NetLinkCore &operator=(const NetLinkCore &) = delete;

	// Call before init()
	void					configure(const NetLinkConfig &config, const NetLinkCallbacks &callbacks);

	bool					init();
	void					shutdown();

	// Local interface address all networking runs on (the selected network adapter)
	void					setLocalAddress(const std::string &ipv4);

	bool					startDiscovery();
	void					stopDiscovery();

	std::vector<Endpoint>	getPotentialEndpoints();

	bool					connectTo(const Endpoint &remote);
	void					respondToConnection(bool accepted);
	void					disconnect();

	ConnectionState			getConnectionState() const { return mState.load(); }

	bool					send(uint32_t type, const std::vector<uint8_t> &payload, DeliveryMode mode);

	// Queues a public callback invocation onto the event thread. Callbacks never run on internal
	// threads or while internal locks are held, so they may safely call back into NetLink.
	void					postEvent(Event event);

private:
	void					wireServices();

	void					applyLocalAddress();
	void					updateDiscoveryConfig();

	void					onConnectionStatus(const ConnectionStatusUpdate &update);
	void					onValidationResult(const ValidationResult &result);

	void					emitConnectionChanged(ConnectionState state, const std::string &message, const DiscoveryEndpoint &remote);

	static Endpoint			toPublicEndpoint(const DiscoveryEndpoint &endpoint);


	NetLinkConfig						mConfig;
	NetLinkCallbacks					mCallbacks;

	std::mutex							mAddressMutex;
	std::string							mLocalAddress;
	std::atomic<bool>					mInitialized{false};
	std::atomic<ConnectionState>		mState{ConnectionState::None};

	TaskQueue							mEvents;

	// Services (declaration order = construction order; dependencies first)
	DiscoveryService					mDiscovery;
	SignalingService					mSignaling;
	TransportKind						mTransportKind{TransportKind::Tcp};
	std::unique_ptr<ITransportFactory>	mTransportFactory;
	PeerValidationService				mValidation;
	ConnectionService					mConnectionService;
	RemoteCommunication					mCommunication;
};

} // namespace netlink
