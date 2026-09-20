/*
  ==============================================================================
	Module:         SignalingService
	Description:    Verification of the remote before establishing a connection
  ==============================================================================
*/

#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "SignalPacket.h"
#include "ThreadBase.h"
#include "PeerValidation/PeerValidationService.h"
#include "Socket/IDatagramSocket.h"


namespace netlink
{

// Connection lifecycle signals (consumed by ConnectionService)
struct SignalingConnectionCallbacks
{
	std::function<void(const std::string &computerName)>				onConnectRequested;
	std::function<void(const std::string &computerName, bool accepted)> onConnectRequestAnswered;
	std::function<void(const std::string &computerName)>				onDisconnectReceived;
	std::function<void(const std::string &computerName)>				onReadyFlagReceived;
	std::function<void(const std::string &computerName, int dataPort)>	onDataPortReceived;
};


// Peer validation signals (consumed by PeerValidationService)
struct SignalingValidationCallbacks
{
	std::function<void(const std::string &computerName, RemoteRequest request)>		 onValidationRequestReceived;
	std::function<void(const std::string &computerName, const std::string &secret)>	 onSecretResponseReceived;
	std::function<void(const std::string &computerName, const std::string &version)> onVersionResponseReceived;
	std::function<void(const std::string &computerName)>							 onValidationHandshakeReceived;
};

using SocketBoundCallback = std::function<void(int boundPort)>;


struct PeerEndpoint
{
	net::IPv4Address IPv4{};
	int				 signalingPort{0};

	bool			 isValid() const { return !IPv4.isUnspecified() && signalingPort != 0; }
};


class SignalingService : private ThreadBase
{
public:
	explicit SignalingService(net::DatagramSocketFactory socketFactory = {});
	~SignalingService() override;
	SignalingService(const SignalingService &)			  = delete;
	SignalingService &operator=(const SignalingService &) = delete;

	bool			  init(const std::string &localComputerName);
	void			  deinit();

	// Binds the signaling socket to the adapter address
	void			  setLocalIPv4(const net::IPv4Address &localIPv4);

	// Receive loop
	using ThreadBase::start;
	using ThreadBase::stop;

	int	 getBoundPort() const { return mBoundPort.load(); }

	// Set before start(). Invoked on the signaling thread.
	void setConnectionCallbacks(SignalingConnectionCallbacks cb) { mConnectionCallbacks = std::move(cb); }
	void setValidationCallbacks(SignalingValidationCallbacks cb) { mValidationCallbacks = std::move(cb); }
	void setOnSocketBound(SocketBoundCallback cb) { mOnSocketBound = std::move(cb); }

	// Peer registry
	void registerPeer(const std::string &displayName, const net::IPv4Address &ipv4, const int signalingPort);
	void unregisterPeer(const std::string &displayName);

	void sendConnectRequest(const std::string &computerName);
	void sendConnectAnswer(const std::string &computerName, bool requestAccepted);
	void sendDisconnect(const std::string &computerName);
	void sendReadyFlag(const std::string &computerName);
	void sendDataPort(const std::string &computerName, int dataPort);

	// Validation signaling (called via PeerValidationSendCallbacks)
	void sendValidationRequest(const std::string &computerName, RemoteRequest request);
	void sendSecretResponse(const std::string &computerName, const std::string &secret);
	void sendVersionResponse(const std::string &computerName, const std::string &version);
	void sendValidationHandshake(const std::string &computerName);

private:
	PeerEndpoint						  resolvePeer(const std::string &computerName) const;

	void								  run() override;
	void								  sendPacket(const PeerEndpoint &endpoint, const SignalPacket &packet);
	void								  receivePackage();
	void								  routePacket(const SignalPacket &packet);

	SignalPacket						  makeEnvelope(SignalType type) const;

	std::shared_ptr<net::IDatagramSocket> socket() const;


	net::DatagramSocketFactory			  mSocketFactory;

	mutable std::mutex					  mSocketMutex;
	std::shared_ptr<net::IDatagramSocket> mSocket;
	std::string							  mLocalComputerName;
	net::IPv4Address					  mLocalIPv4;
	std::atomic<int>					  mBoundPort{0};

	std::vector<uint8_t>				  mReceiveBuffer; // signaling thread only

	std::atomic<bool>					  mInitialized{false};
	SignalingConnectionCallbacks		  mConnectionCallbacks;
	SignalingValidationCallbacks		  mValidationCallbacks;
	SocketBoundCallback					  mOnSocketBound;

	std::map<std::string, PeerEndpoint>	  mPeerRegistry; // key = displayName
	mutable std::mutex					  mPeerRegistryMutex;
};

} // namespace netlink
