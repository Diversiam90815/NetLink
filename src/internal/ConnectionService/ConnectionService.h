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

#include "ConnectionConfig.h"
#include "ConnectionPhase.h"
#include "PeerValidationService.h"
#include "Discovery/DiscoveryEndpoint.h"
#include "Signaling/SignalingService.h"
#include "TimeoutService/TimeoutService.h"
#include "Transport/TransportFactory.h"
#include "Transport/TransportInterfaces.h"
#include "Signaling/RoleNegotiation.h"


namespace netlink
{

struct ConnectionServiceCallbacks
{
	std::function<void(const DiscoveryEndpoint &remote)> onConnectionRequested; // Inbound request arrived: app must call respondToConnection
	std::function<void(ISession::pointer session)>		 onConnected;			// Both peers confirmed ready: session is ready for messaging
	std::function<void(const std::string &reason)>		 onConnectionFailed;	// Connection failed (declined, timeout, transport error, validation)
	std::function<void()>								 onDisconnected;		// Remote initiated a disconnect
};


struct PeerState
{
	ConnectionPhase			 phase{ConnectionPhase::Idle};
	DiscoveryEndpoint		 remote{};
	bool					 isInitiator{false};
	bool					 localReadyFlag{false};
	bool					 remoteReadyFlag{false};
	ISession::pointer		 session{};
	std::unique_ptr<IServer> server;
	std::unique_ptr<IClient> client;
};


class ConnectionService
{
public:
	ConnectionService(asio::io_context &ioContext, SignalingService &signaling, ITransportFactory &transportFactory, PeerValidationService &validation);

	void			setCallbacks(ConnectionServiceCallbacks cb);
	void			setConfig(const ConnectionConfig &config);
	void			setLocalIP(const std::string &ip);

	// Initiator path, called by NetLink::connectTo()
	void			requestConnection(const DiscoveryEndpoint &remote);

	// Responder path, called by NetLink::respondToConnection()
	void			respondToConnection(bool accepted);

	// Tear down active connection or in-progress attempt
	void			disconnect();

	ConnectionPhase getPhase() const;

private:
	// Signal handlers, wired in constructor, called from SignalingService IO thread
	void							 onSignalConnectRequested(const SignalPacket &pkt);
	void							 onSignalConnectAccepted(const SignalPacket &pkt);
	void							 onSignalConnectDeclined(const SignalPacket &pkt);
	void							 onSignalDisconnect(const SignalPacket &pkt);
	void							 onSignalReadyFlag(const SignalPacket &pkt);

	// Per-peer flow stages — caller must hold mMutex
	void							 beginTransportEstablishment(PeerState &peer);
	void							 sendLocalReadyFlag(PeerState &peer);
	void							 checkBothReady(PeerState &peer);

	// Timeout expiry handler
	void							 onTimeout(const TimeoutKey &key);

	// Reset all state to Idle
	void							 reset();


	// Dependencies
	asio::io_context				&mIoContext;
	SignalingService				&mSignaling;
	ITransportFactory				&mTransportFactory;
	PeerValidationService			&mValidation;

	// Configuration and callbacks
	ConnectionConfig				 mConfig;
	ConnectionServiceCallbacks		 mCallbacks;
	std::string						 mLocalIP{};

	// Per-peer state keyed by IP
	std::map<std::string, PeerState> mPeers;

	// Timeout management
	TimeoutService					 mTimeoutService;

	// Thread safety
	mutable std::mutex				 mMutex;
};

} // namespace netlink
