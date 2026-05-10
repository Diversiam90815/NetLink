/*
  ==============================================================================
	Module:         ConnectionService
	Description:    Orchestrates the full connection lifecycle with per-phase
					timeouts, ReadyFlag synchronization, and peer validation.
  ==============================================================================
*/

#include "ConnectionService.h"
#include "NetLinkLog.h"


netlink::ConnectionService::ConnectionService(asio::io_context &ioContext, SignalingService &signaling, ITransportFactory &transportFactory)
	: mIoContext(ioContext), mSignaling(signaling), mTransportFactory(transportFactory)
{
	SignalingCallbacks cb;
	//cb.onConnectRequested	= [this](const SignalPacket &pkt) { onSignalConnectRequested(pkt); };
	//cb.onConnectAccepted	= [this](const SignalPacket &pkt) { onSignalConnectAccepted(pkt); };
	//cb.onConnectDeclined	= [this](const SignalPacket &pkt) { onSignalConnectDeclined(pkt); };
	//cb.onDisconnectReceived = [this](const SignalPacket &pkt) { onSignalDisconnect(pkt); };
	//cb.onReadyFlagReceived	= [this](const SignalPacket &pkt) { onSignalReadyFlag(pkt); };
	mSignaling.setCallbacks(std::move(cb));
}


bool netlink::ConnectionService::initiateConnection(const std::string &computerName)
{
	return false;
}


bool netlink::ConnectionService::acceptIncomingConnection(const std::string &computerName)
{
	return false;
}


bool netlink::ConnectionService::declineIncomingConnection(const std::string &computerName)
{
	return false;
}


bool netlink::ConnectionService::closeConnection(const std::string &computerName)
{
	return false;
}


bool netlink::ConnectionService::hasIncomingInvitation() const
{
	return false;
}


std::optional<DiscoveryEndpoint> netlink::ConnectionService::getCurrentRemote() const
{
	return std::optional<DiscoveryEndpoint>();
}


std::optional<netlink::ConnectionRequest> netlink::ConnectionService::getCurrentRequest() const
{
	return std::optional<ConnectionRequest>();
}


netlink::ConnectionPhase netlink::ConnectionService::getConnectionPhase() const
{
	return ConnectionPhase();
}


void							netlink::ConnectionService::onPeerValidated(const ValidationResult &peerValidation) {}


void							netlink::ConnectionService::clearCurrentConnection() {}


void							netlink::ConnectionService::resetConnectionFlags() {}


std::optional<netlink::ValidationResult> netlink::ConnectionService::findValidationResult(const std::string &computerName) const
{
	return std::optional<ValidationResult>();
}


std::optional<netlink::ValidationResult> netlink::ConnectionService::findValidationResultByIPv4(const std::string &ipv4) const
{
	return std::optional<ValidationResult>();
}


bool netlink::ConnectionService::retryConnection()
{
	return false;
}


void		netlink::ConnectionService::notifyStatus(ConnectionStatusUpdate::Type type, const std::string &message, bool success) {}


netlink::SessionRole netlink::ConnectionService::determineLocalSessionRole()
{
	return SessionRole();
}


void netlink::ConnectionService::onTimeout(const TimeoutKey &key) {}
