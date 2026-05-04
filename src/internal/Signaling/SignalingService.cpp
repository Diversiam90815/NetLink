/*
  ==============================================================================
	Module:         SignalingService
	Description:    Verification of the remote before establishing a connection
  ==============================================================================
*/

#include "SignalingService.h"


netlink::SignalingService::SignalingService(asio::io_context &ioContext) {}


netlink::SignalingService::~SignalingService() {}


bool netlink::SignalingService::init(const std::string &localIPv4)
{
	return false;
}


void netlink::SignalingService::deinit() {}


void netlink::SignalingService::sendConnectRequest(const std::string &targetIP, int targetSignalingPort, const std::string &displayName) {}


void netlink::SignalingService::sendConnectAccept(const std::string &targetIP, int targetSignalingPort) {}


void netlink::SignalingService::sendConnectDecline(const std::string &targetIP, int targetSignalingPort) {}


void netlink::SignalingService::sendDisconnect(const std::string &targetIP, int targetSignalingPort) {}


void netlink::SignalingService::run() {}


void netlink::SignalingService::receiveAsync() {}


void netlink::SignalingService::handleReceive(const asio::error_code &error, size_t bytesReceived) {}


void netlink::SignalingService::routePacket(const SignalPacket &packet) {}
