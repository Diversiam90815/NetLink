/*
  ==============================================================================
	Module:         ConnectionService
	Description:    Orchestrates the full connection lifecycle with per-state
					timeouts, ReadyFlag synchronization, and peer validation.
  ==============================================================================
*/

#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "ConnectionPhase.h"
#include "ReadySyncTracker.h"
#include "Channel/PeerChannel.h"
#include "PeerValidation/ValidationResult.h"
#include "PeerValidation/ValidatedPeerRegistry.h"
#include "Discovery/DiscoveryEndpoint.h"
#include "TimeoutService/TimeoutService.h"
#include "Util/TaskQueue.h"


namespace netlink
{

struct ConnectionServiceCallbacks
{
	std::function<void(const ConnectionStatusUpdate &update)> onStatusUpdate;
};


namespace ConnectionTimeouts
{
constexpr auto Invitation = "invitation";
constexpr auto ReadyFlag  = "ready_flag";
} // namespace ConnectionTimeouts


struct ConnectionConfig
{
	bool autoAcceptConnection{false}; // If enabled, an incoming connection is automatically enabled if valid

	int	 invitationTimeoutMs{5000};
	int	 readyFlagTimeoutMs{5000};	  // from accepting until the remote's ready flag arrived
};


struct ConnectionRequest
{
	ConnectionStateInternal				  state{ConnectionStateInternal::Idle};
	ValidationResult					  validationResult;

	DiscoveryEndpoint					  remote{};
	bool								  isInitiator{false};

	std::chrono::steady_clock::time_point requestTime;
	std::chrono::steady_clock::time_point lastActivityTime;

	bool								  localReadyFlag{false};
	bool								  remoteReadyFlag{false};
};


class ConnectionService
{
public:
	explicit ConnectionService(PeerChannel &channel);
	~ConnectionService();

	// Configuration
	void							 setCallbacks(ConnectionServiceCallbacks cb) { mCallbacks = std::move(cb); }
	void							 setConfig(const ConnectionConfig &config);
	void							 setLocalIP(const net::IPv4Address &ip);

	// Connection management
	bool							 initiateConnection(const std::string &computerName);
	bool							 acceptIncomingConnection(const std::string &computerName);
	bool							 declineIncomingConnection(const std::string &computerName, const std::string &reason = "");
	bool							 closeConnection(const std::string &computerName);

	// State
	bool							 isConnected() const { return mConnected; }
	bool							 isConnecting() const { return mConnecting; }
	bool							 hasIncomingInvitation() const;

	std::optional<DiscoveryEndpoint> getCurrentRemote() const;
	ConnectionStateInternal			 getConnectionState() const;

	// Sending helper
	bool							 sendConnectionInvitation(const std::string &computerName) const;
	bool							 sendDisconnectMessage(const std::string &computerName) const;
	bool							 answerInvitation(const std::string &computerName, const bool connectionAccepted, const std::string &reason = "") const;
	bool							 sendConnectionReadyFlag(const std::string &computerName, const bool flag) const;

	// Signals from the remote (thread-safe, wired to PeerChannel by the owner)
	void							 onDisconnectReceived(const std::string &computerName);
	void							 onReadyFlagReceived(const std::string &computerName);

	// Receiving helper
	void							 onReceivedInvitation(const std::string &computerName);
	void							 onReceivedAnswerToInvite(const std::string &computerName, const bool connectionAccepted, const std::string &reason);
	void							 onReceivedConnectionReadyFlag(const std::string &computerName);

	// Peer validated
	void							 onPeerValidated(const ValidationResult &peerValidation);

	// The peer channel lost the remote (unacknowledged messages, silence, restart)
	void							 onPeerLost(const std::string &computerName, const std::string &reason);

private:
	// State management
	void							 clearCurrentConnection();

	// Helper methods
	void							 notifyStatus(ConnectionStatusUpdate::Type type, const std::string &message = "", bool success = true) const;
	void							 notifyStatus(ConnectionStatusUpdate update) const;

	// Both sides after the invitation was accepted: announce readiness, complete once the remote is ready too
	bool							 openSession();
	void							 completeSessionIfReady();

	// Timeouts (caller must hold mConnectingMutex). Expiry is handled on the task queue.
	void							 armTimeout(const TimeoutKey &key, int timeoutMs);
	void							 disarmTimeouts(const std::function<bool(const TimeoutKey &)> &matches);
	void							 disarmAllTimeouts();
	void							 onTimeout(const TimeoutKey &key);

	// Dependencies
	PeerChannel						&mChannel;

	// Configuration and callbacks
	ConnectionConfig				 mConfig;
	ConnectionServiceCallbacks		 mCallbacks;
	net::IPv4Address				 mLocalIP{};

	TaskQueue						 mTaskQueue;
	ValidatedPeerRegistry			 mValidatedPeers;
	ReadySyncTracker				 mReadySync;
	TimeoutService					 mTimeoutService;
	std::map<TimeoutKey, uint64_t>	 mArmedTimeouts; // generation per armed timeout, guarded by mConnectingMutex
	uint64_t						 mTimeoutGeneration{0};

	// State
	std::atomic<bool>				 mConnected{false};
	std::atomic<bool>				 mConnecting{false};
	std::optional<ConnectionRequest> mCurrentRequest;
	mutable std::mutex				 mConnectingMutex;
};

} // namespace netlink
