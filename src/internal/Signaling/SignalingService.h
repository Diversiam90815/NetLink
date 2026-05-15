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
	std::function<void(const SignalPacket &)> onConnectRequested;
	std::function<void(const SignalPacket &)> onConnectAccepted;
	std::function<void(const SignalPacket &)> onConnectDeclined;
	std::function<void(const SignalPacket &)> onDisconnectReceived;
	std::function<void(const SignalPacket &)> onReadyFlagReceived;
	std::function<void(const SignalPacket &)> onDataPortReceived;
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

	bool init(const std::string &localIPv4);
	void deinit();

	int	 getBoundPort() const { return mBoundPort; }

	void setCallbacks(SignalingCallbacks cb) { mCallbacks = std::move(cb); }

	// Peer registry
	void registerPeer(const std::string &displayName, const std::string &ipv4, const int signalingPort);
	void unregisterPeer(const std::string &displayName);

	void sendConnectRequest(const std::string &computerName);
	void sendConnectAccept(const std::string &computerName);
	void sendConnectDecline(const std::string &computerName);
	void sendDisconnect(const std::string &computerName);
	void sendReadyFlag(const std::string &computerName);

	// Validation signaling (called via PeerValidationSendCallbacks)
	void sendValidationRequest(const std::string &computerName, RemoteRequest request);
	void sendSecretResponse(const std::string &computerName, const std::string &secret);
	void sendVersionResponse(const std::string &computerName, const std::string &version);
	void sendValidationHandshake(const std::string &computerName);

private:
	PeerEndpoint						resolvePeer(const std::string &displayName) const;

	void								run() override;
	void								receiveAsync();
	void								handleReceive(const asio::error_code &error, size_t bytesReceived);
	void								routePacket(const SignalPacket &packet);
	void								sendPacket(const std::string &targetIP, int targetPort, const SignalPacket &packet);

	asio::io_context				   *mIoContext{nullptr};
	udp::socket							mSocket;
	udp::endpoint						mSenderEndpoint;
	std::array<char, 1024>				mRecvBuffer{};
	int									mBoundPort{0};
	std::atomic<bool>					mInitialized{false};
	SignalingCallbacks					mCallbacks;

	std::map<std::string, PeerEndpoint> mPeerRegistry; // key = displayName
	mutable std::mutex					mPeerRegistryMutex;
};

} // namespace netlink
