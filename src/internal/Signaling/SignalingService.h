/*
  ==============================================================================
	Module:         SignalingService
	Description:    Verification of the remote before establishing a connection
  ==============================================================================
*/

#pragma once

#include <asio.hpp>
#include <functional>
#include <string>
#include <atomic>
#include <array>

#include "SignalPacket.h"
#include "ThreadBase.h"
#include "PeerValidation/PeerValidationService.h"


using asio::ip::udp;

namespace netlink
{

struct SignalingCallbacks
{
	std::function<void(const std::string &computerName)>							 onConnectRequested;
	std::function<void(const std::string &computerName, bool accepted)>				 onConnectRequestAnswered;
	std::function<void(const std::string &computerName)>							 onDisconnectReceived;
	std::function<void(const std::string &computerName)>							 onReadyFlagReceived;
	std::function<void(const std::string &computerName, int dataPort)>				 onDataPortReceived;

	// Validation
	std::function<void(const std::string &computerName, uint8_t request)>			 onValidationRequestReceived;
	std::function<void(const std::string &computerName, const std::string &secret)>	 onSecretResponseReceived;
	std::function<void(const std::string &computerName, const std::string &version)> onVersionResponseReceived;
	std::function<void(const std::string &computerName)>							 onValidationHandshakeReceived;
};


struct PeerEndpoint
{
	std::string IPv4{};
	int			signalingPort{0};

	bool		isValid() const { return !IPv4.empty() && signalingPort != 0; }
};


class SignalingService : public ThreadBase
{
public:
	explicit SignalingService(asio::io_context &ioContext);
	~SignalingService();

	bool init(const std::string &localComputerName);
	void deinit();
	void setLocalIPv4(const std::string &localIPv4);

	int	 getBoundPort() const { return mBoundPort; }

	void setCallbacks(SignalingCallbacks cb) { mCallbacks = std::move(cb); }

	// Peer registry
	void registerPeer(const std::string &displayName, const std::string &ipv4, const int signalingPort);
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
	PeerEndpoint						resolvePeer(const std::string &computerName) const;

	void								run() override;
	void								receiveAsync();
	void								handleReceive(const asio::error_code &error, size_t bytesReceived);
	void								routePacket(const SignalPacket &packet);
	void								sendPacket(const PeerEndpoint &endpoint, const SignalPacket &packet);

	SignalPacket						makeEnvelope(SignalType type) const;


	asio::io_context				   *mIoContext{nullptr};
	udp::socket							mSocket;
	udp::endpoint						mSenderEndpoint;
	std::array<char, 1024>				mRecvBuffer{};

	std::string							mLocalComputerName;
	std::string							mLocalIPv4;
	int									mBoundPort{0};

	std::atomic<bool>					mInitialized{false};
	SignalingCallbacks					mCallbacks;

	std::map<std::string, PeerEndpoint> mPeerRegistry; // key = displayName
	mutable std::mutex					mPeerRegistryMutex;
};

} // namespace netlink
