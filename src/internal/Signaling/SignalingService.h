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

	void sendConnectRequest(const std::string &targetIP, int targetSignalingPort, const std::string &displayName);
	void sendConnectAccept(const std::string &targetIP, int targetSignalingPort);
	void sendConnectDecline(const std::string &targetIP, int targetSignalingPort);
	void sendDisconnect(const std::string &targetIP, int targetSignalingPort);
	void sendReadyFlag(const std::string &targetIP, int targetSignalingPort);

private:
	void				   run() override;
	void				   receiveAsync();
	void				   handleReceive(const asio::error_code &error, size_t bytesReceived);
	void				   routePacket(const SignalPacket &packet);
	void				   sendPacket(const std::string &targetIP, int targetPort, const SignalPacket &packet);

	asio::io_context	  *mIoContext{nullptr};
	udp::socket			   mSocket;
	udp::endpoint		   mSenderEndpoint;
	std::array<char, 1024> mRecvBuffer{};
	int					   mBoundPort{0};
	std::atomic<bool>	   mInitialized{false};
	SignalingCallbacks	   mCallbacks;
};

} // namespace netlink
