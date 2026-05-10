/*
  ==============================================================================
	Module:         ConnectionService
	Description:    Orchestrates the full connection lifecycle with per-state
					timeouts, ReadyFlag synchronization, and peer validation.
  ==============================================================================
*/

#include "ConnectionService.h"
#include "NetLinkLog.h"


netlink::ConnectionService::ConnectionService(asio::io_context &ioContext, SignalingService &signaling, ITransportFactory &transportFactory)
	: mIoContext(ioContext), mSignaling(signaling), mTransportFactory(transportFactory)
{
	SignalingCallbacks cb;
	// cb.onConnectRequested	= [this](const SignalPacket &pkt) { onSignalConnectRequested(pkt); };
	// cb.onConnectAccepted	= [this](const SignalPacket &pkt) { onSignalConnectAccepted(pkt); };
	// cb.onConnectDeclined	= [this](const SignalPacket &pkt) { onSignalConnectDeclined(pkt); };
	// cb.onDisconnectReceived = [this](const SignalPacket &pkt) { onSignalDisconnect(pkt); };
	// cb.onReadyFlagReceived	= [this](const SignalPacket &pkt) { onSignalReadyFlag(pkt); };
	mSignaling.setCallbacks(std::move(cb));
}


bool netlink::ConnectionService::initiateConnection(const std::string &computerName)
{
	std::lock_guard<std::mutex> lock(mConnectingMutex);

	if (isConnected())
	{
		NETLINK_LOG_WARNING("Already connected to a device!");
		return false;
	}

	if (isConnecting())
	{
		NETLINK_LOG_WARNING("Connection already in progress!");
		return false;
	}

	NETLINK_LOG_INFO("Initiating connection to: {}", computerName);

	// Get validation result
	auto validationResult = findValidationResult(computerName);
	if (!validationResult.has_value())
	{
		NETLINK_LOG_ERROR("No validation result for: {}", computerName);
		return false;
	}

	// Check if validation allows connection
	if (!validationResult->canConnect)
	{
		NETLINK_LOG_WARNING("Validation result not ready for connection: {}", validationResult->message);
		return false;
	}

	// Create connection request
	ConnectionRequest request;
	request.remote			 = validationResult->remoteEndpoint;
	request.validationResult = validationResult.value();
	request.requestTime		 = std::chrono::steady_clock::now();
	request.lastActivityTime = request.requestTime;
	request.isInitiator		 = true;
	request.state			 = ConnectionState::Initiated;

	mCurrentRequest			 = std::move(request);
	mConnecting.store(true);
	mConnectionAttempts = 0;

	// send invitation
	notifyStatus(ConnectionStatusUpdate::Type::Initiated, "Connection initiated to " + computerName);

	if (!sendConnectionInvitation(computerName))
	{
		NETLINK_LOG_ERROR("Failed to send connection invitation");
		notifyStatus(ConnectionStatusUpdate::Type::Failed, "Failed to send invitation", false);
		clearCurrentConnection();
		return false;
	}

	notifyStatus(ConnectionStatusUpdate::Type::InvitationSent, "Invitation sent to " + computerName);

	// update current state
	mCurrentRequest->state = ConnectionState::InvitationSent;

	NETLINK_LOG_INFO("Connection invitation sent to {}", computerName);
	return true;
}


bool netlink::ConnectionService::acceptIncomingConnection(const std::string &computerName)
{
	std::lock_guard<std::mutex> lock(mConnectingMutex);

	if (isConnected())
	{
		NETLINK_LOG_WARNING("Already connected");
		return false;
	}

	if (!mCurrentRequest.has_value() || mCurrentRequest->state != ConnectionState::InvitationReceived)
	{
		NETLINK_LOG_WARNING("No incoming invitation to accept");
		return false;
	}

	if (mCurrentRequest->remote.displayName != computerName)
	{
		NETLINK_LOG_WARNING("Remote missmatch: expected {}, got {}", mCurrentRequest->remote.displayName, computerName);
		return false;
	}

	NETLINK_LOG_INFO("Accepting invitation from {}", computerName);

	// Start connection timeout
	mTimeoutService.startTimeout({ConnectionTimeouts::Connection, computerName}, mConfig.connectionTimeoutMs, [this](const TimeoutKey &key) { onTimeout(key); });

	// send acceptance
	notifyStatus(ConnectionStatusUpdate::Type::Accepted, "Invitation from " + computerName + " accepted");

	if (answerInvitation(computerName, true))
	{
		NETLINK_LOG_ERROR("Failed to send acceptance!");
		notifyStatus(ConnectionStatusUpdate::Type::Failed, "Failed to send connection acceptance", false);
		clearCurrentConnection();
		return false;
	}

	mCurrentRequest->state = ConnectionState::EstablishingTransport;

	if (determineLocalSessionRole())
	{
		NETLINK_LOG_ERROR("Failed to start transport role establishing");
		notifyStatus(ConnectionStatusUpdate::Type::Failed, "Failed to start transport role establishing", false);
		clearCurrentConnection();
		return false;
	}

	return true;
}


bool netlink::ConnectionService::declineIncomingConnection(const std::string &computerName, const std::string &reason)
{
	std::lock_guard<std::mutex> lock(mConnectingMutex);

	if (!mCurrentRequest.has_value() || mCurrentRequest->state != ConnectionState::InvitationReceived)
	{
		NETLINK_LOG_WARNING("No incoming invitation to decline");
		return false;
	}

	NETLINK_LOG_INFO("Declining connection from {}: {}", computerName, reason.empty() ? "No reason given." : reason);

	answerInvitation(computerName, false, reason);
	notifyStatus(ConnectionStatusUpdate::Type::Declined, reason);

	clearCurrentConnection();
	return true;
}


bool netlink::ConnectionService::closeConnection(const std::string &computerName)
{
	std::lock_guard<std::mutex> lock(mConnectingMutex);

	if (!mCurrentRequest.has_value())
	{
		NETLINK_LOG_WARNING("No connection to close");
		return false;
	}

	std::string remote = computerName;

	// if computerName was left empty, we close the current connection
	if (computerName.empty())
		remote = mCurrentRequest->remote.displayName;

	// validate remote's name unless empty (alwas force close if left empty)
	if (!computerName.empty() && mCurrentRequest->remote.displayName != computerName)
	{
		NETLINK_LOG_WARNING("Connection close missmatch: request: {}, current: {}", computerName, mCurrentRequest->remote.displayName);
		return false;
	}

	// cancel all timeouts for this connection
	if (!remote.empty())
		mTimeoutService.cancelByIdentifier(remote);
	else
		mTimeoutService.cancelAll();

	// only proceed if we have an actual connection in progress or established
	if (!isConnected() || !isConnecting())
	{
		NETLINK_LOG_DEBUG("Connection already idle, clearing state");
		clearCurrentConnection();
		return true;
	}

	NETLINK_LOG_INFO("Closing current connection to {}", remote);

	mCurrentRequest->state = ConnectionState::Disconnecting;

	notifyStatus(ConnectionStatusUpdate::Type::Closing, "Closing connection");

	sendDisconnectMessage(computerName);

	notifyStatus(ConnectionStatusUpdate::Type::Closed, "Connection closed");
	clearCurrentConnection();
	return true;
}


bool netlink::ConnectionService::hasIncomingInvitation() const
{
	std::lock_guard<std::mutex> lock(mConnectingMutex);

	return mCurrentRequest.has_value() && mCurrentRequest->state == ConnectionState::InvitationReceived;
}


std::optional<DiscoveryEndpoint> netlink::ConnectionService::getCurrentRemote() const
{
	std::lock_guard<std::mutex> lock(mConnectingMutex);

	if (mCurrentRequest.has_value())
		return mCurrentRequest->remote;

	return std::nullopt;
}


netlink::ConnectionState netlink::ConnectionService::getConnectionState() const
{
	std::lock_guard<std::mutex> lock(mConnectingMutex);

	if (mCurrentRequest.has_value())
		return mCurrentRequest->state;

	return ConnectionState::Idle;
}


bool netlink::ConnectionService::sendConnectionInvitation(const std::string &computerName)
{
	return false;
}


bool netlink::ConnectionService::sendDisconnectMessage(const std::string &computerName)
{
	return false;
}


bool netlink::ConnectionService::answerInvitation(const std::string &computerName, const bool connectionAccepted, const std::string &reason)
{
	return false;
}


bool netlink::ConnectionService::sendConnectionReadyFlag(const std::string &computerName, const bool flag)
{
	return false;
}


void netlink::ConnectionService::onReceivedInvitation(const std::string &computerName) {}


void netlink::ConnectionService::onReceivedAnswerToInvite(const std::string &computerName, const bool connectionAccepted, const std::string &reason)
{

	std::lock_guard<std::mutex> lock(mConnectingMutex);

	if (!mCurrentRequest.has_value())
	{
		NETLINK_LOG_WARNING("Received acceptance but no connection request exists!");
		return;
	}

	if (mCurrentRequest->state != ConnectionState::InvitationSent)
	{
		NETLINK_LOG_WARNING("Received acceptance in unexpected state: {}", static_cast<int>(mCurrentRequest->state));
		return;
	}

	// cancel invitation timeout
	mTimeoutService.cancelTimeout({ConnectionTimeouts::Invitation, computerName});

	if (connectionAccepted)
	{
		NETLINK_LOG_INFO("Connection accepted by {}", computerName);

		mCurrentRequest->state			  = ConnectionState::Accepted;
		mCurrentRequest->lastActivityTime = std::chrono::steady_clock::now();

		notifyStatus(ConnectionStatusUpdate::Type::Accepted, "Connection accepted by " + computerName);

		// start connection timeout
		mTimeoutService.startTimeout({ConnectionTimeouts::Connection, computerName}, mConfig.connectionTimeoutMs, [this](const TimeoutKey &key) { onTimeout(key); });

		// start role negotiation
		if (!determineLocalSessionRole())
		{
			NETLINK_LOG_ERROR("Failed to start role negotiation!");
			notifyStatus(ConnectionStatusUpdate::Type::Failed, "Session role negotiation failed", false);
			clearCurrentConnection();
		}
	}
	else
	{
		NETLINK_LOG_WARNING("Connection declined by {}", computerName);

		notifyStatus(ConnectionStatusUpdate::Type::Declined, "Connection declined by " + computerName, false);
		clearCurrentConnection();
	}
}


void netlink::ConnectionService::onReceivedConnectionReadyFlag(const std::string &computerName) {}


void netlink::ConnectionService::onPeerValidated(const ValidationResult &peerValidation)
{
	NETLINK_LOG_INFO("Peer {} validated", peerValidation.remoteEndpoint.displayName);

	// store the result
	{
		std::lock_guard<std::mutex> lock(mValidationMutex);
		mValidatedResults[peerValidation.remoteEndpoint.displayName] = peerValidation;
	}
}


void netlink::ConnectionService::clearCurrentConnection()
{
	NETLINK_LOG_INFO("Clearing current connection..");

	mCurrentRequest.reset();
	mConnected.store(false);
	mConnecting.store(false);
	mConnectionAttempts = 0;
	mTimeoutService.cancelAll();

	mLocalReady.store(false);
	mRemoteReady.store(false);
}


std::optional<netlink::ValidationResult> netlink::ConnectionService::findValidationResult(const std::string &computerName) const
{
	std::lock_guard<std::mutex> lock(mConnectingMutex);

	auto						it = mValidatedResults.find(computerName);

	if (it != mValidatedResults.end())
		return it->second;

	return std::nullopt;
}


std::optional<netlink::ValidationResult> netlink::ConnectionService::findValidationResultByIPv4(const std::string &ipv4) const
{
	std::lock_guard<std::mutex> lock(mConnectingMutex);

	for (const auto &[computerName, result] : mValidatedResults)
	{
		if (result.remoteEndpoint.IPAddress == ipv4)
			return result;
	}

	return std::nullopt;
}


bool netlink::ConnectionService::retryConnection()
{
	return false;
}


void netlink::ConnectionService::notifyStatus(ConnectionStatusUpdate::Type type, const std::string &message, bool success) {}


bool netlink::ConnectionService::determineLocalSessionRole()
{
	return false;
}


void netlink::ConnectionService::onTimeout(const TimeoutKey &key) {}
