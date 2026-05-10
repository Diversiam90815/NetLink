/*
  ==============================================================================
	Module:         ConnectionService
	Description:    Orchestrates the full connection lifecycle with per-phase
					timeouts, ReadyFlag synchronization, and peer validation.
  ==============================================================================
*/

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "ConnectionPhase.h"
#include "RoleNegotiation.h"
#include "PeerValidation/PeerValidationService.h"
#include "Discovery/DiscoveryEndpoint.h"
#include "Signaling/SignalingService.h"
#include "TimeoutService/TimeoutService.h"
#include "Transport/TransportFactory.h"
#include "Transport/TransportInterfaces.h"


namespace netlink
{

struct ConnectionServiceCallbacks
{
	std::function<void(const DiscoveryEndpoint &remote)> onConnectionRequested; // Inbound request arrived: app must call respondToConnection
	std::function<void(ISession::pointer session)>		 onConnected;			// Both peers confirmed ready: session is ready for messaging
	std::function<void(const std::string &reason)>		 onConnectionFailed;	// Connection failed (declined, timeout, transport error, validation)
	std::function<void()>								 onDisconnected;		// Remote initiated a disconnect
};


namespace ConnectionTimeouts
{
constexpr const char *Connection = "connection";
constexpr const char *Invitation = "invitation";
constexpr const char *ReadyFlag	 = "ready_flag";
} // namespace ConnectionTimeouts


struct ConnectionConfig
{
	int						  maxConnectionRetries{3};
	std::chrono::milliseconds retryDelay{300};
	bool					  autoAcceptConnection{false}; // If enabled, an incoming connection is automatically enabled if valid

	int						  invitationTimeoutMs{5000};
	int						  connectionTimeoutMs{10000};
	int						  readyFlagTimeoutMs{3000};
};


struct ConnectionRequest
{
	ConnectionPhase			 phase{ConnectionPhase::Idle};

	DiscoveryEndpoint		 remote{};
	bool					 isInitiator{false};
	SessionRole				 localRole;

	bool					 localReadyFlag{false};
	bool					 remoteReadyFlag{false};

	ISession::pointer		 session{};
	std::unique_ptr<IServer> server;
	std::unique_ptr<IClient> client;
};


class ConnectionService
{
public:
	ConnectionService(asio::io_context &ioContext, SignalingService &signaling, ITransportFactory &transportFactory);

	// Configuration
	void							 setCallbacks(ConnectionServiceCallbacks cb) { mCallbacks = std::move(cb); }
	void							 setConfig(const ConnectionConfig &config) { mConfig = config; }
	void							 setLocalIP(const std::string &ip) { mLocalIP = ip; }

	// Connection management
	bool							 initiateConnection(const std::string &computerName);
	bool							 acceptIncomingConnection(const std::string &computerName);
	bool							 declineIncomingConnection(const std::string &computerName);
	bool							 closeConnection(const std::string &computerName);

	// State
	bool							 isConnected() const { return mConnected; }
	bool							 isConnecting() const { return mConnecting; }
	bool							 hasIncomingInvitation() const;

	std::optional<DiscoveryEndpoint> getCurrentRemote() const;
	std::optional<ConnectionRequest> getCurrentRequest() const;
	ConnectionPhase					 getConnectionPhase() const;

	// Peer validated
	void							 onPeerValidated(const ValidationResult &peerValidation);

private:
	// State management
	void									clearCurrentConnection();
	void									resetConnectionFlags();

	// Helper methods
	std::optional<ValidationResult>			findValidationResult(const std::string &computerName) const;
	std::optional<ValidationResult>			findValidationResultByIPv4(const std::string &ipv4) const;
	bool									retryConnection();
	void									notifyStatus(ConnectionStatusUpdate::Type type, const std::string &message = "", bool success = true);

	// Define role
	SessionRole								determineLocalSessionRole();

	// Timeout expiry handler
	void									onTimeout(const TimeoutKey &key);

	// Dependencies
	asio::io_context					   &mIoContext;
	SignalingService					   &mSignaling;
	ITransportFactory					   &mTransportFactory;

	// Configuration and callbacks
	ConnectionConfig						mConfig;
	ConnectionServiceCallbacks				mCallbacks;
	std::string								mLocalIP{};

	// Validation result cache
	std::map<std::string, ValidationResult> mValidatedResults; // Key : ComputerName
	mutable std::mutex						mValidationMutex;

	// Timeout management
	TimeoutService							mTimeoutService;

	// State
	std::atomic<bool>						mConnected{false};
	std::atomic<bool>						mConnecting{false};
	mutable std::mutex						mConnectingMutex;

	std::atomic<bool>						mLocalReady{false};
	std::atomic<bool>						mRemoteReady{false};
};

} // namespace netlink
