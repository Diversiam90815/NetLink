/*
  ==============================================================================
	Module:         ConnectionService
	Description:    Orchestrates the full connection lifecycle with per-state
					timeouts, ReadyFlag synchronization, and peer validation.
  ==============================================================================
*/

#include "ConnectionService.h"
#include "NetLinkLog.h"


netlink::ConnectionService::ConnectionService(PeerChannel &channel) : mChannel(channel)
{
	mTaskQueue.start();
}


void netlink::ConnectionService::setLocalIP(const net::IPv4Address &ip)
{
	std::lock_guard<std::mutex> lock(mConnectingMutex);
	mLocalIP = ip;
}


void netlink::ConnectionService::onDisconnectReceived(const std::string &computerName)
{
	mTaskQueue.post(
		[this, computerName]()
		{
			std::lock_guard<std::mutex> lock(mConnectingMutex);

			// Only the peer we are dealing with may tear down the connection
			if (!mCurrentRequest.has_value() || mCurrentRequest->remote.displayName != computerName)
				return;

			NETLINK_LOG_INFO("Remote {} disconnected", computerName);

			notifyStatus(ConnectionStatusUpdate::Type::Closed, computerName + " disconnected");
			clearCurrentConnection();
		});
}


void netlink::ConnectionService::onReadyFlagReceived(const std::string &computerName)
{
	mTaskQueue.post(
		[this, computerName]()
		{
			std::lock_guard<std::mutex> lock(mConnectingMutex);
			onReceivedConnectionReadyFlag(computerName);
		});
}


netlink::ConnectionService::~ConnectionService()
{
	{
		std::lock_guard<std::mutex> lock(mConnectingMutex);
		disarmAllTimeouts();
		mCurrentRequest.reset();
	}

	mTaskQueue.stop();
}


void netlink::ConnectionService::setConfig(const ConnectionConfig &config)
{
	mConfig = config;
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
	const auto validationResult = mValidatedPeers.get(computerName);
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
	request.state			 = ConnectionStateInternal::Initiated;

	mCurrentRequest			 = std::move(request);
	mConnecting.store(true);

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
	mCurrentRequest->state = ConnectionStateInternal::InvitationSent;

	// Start timeout waiting for remote to respond to our invitation
	armTimeout({.category = ConnectionTimeouts::Invitation, .identifier = computerName}, mConfig.invitationTimeoutMs);

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

	if (!mCurrentRequest.has_value() || mCurrentRequest->state != ConnectionStateInternal::InvitationReceived)
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

	// The pending invitation is answered now
	disarmTimeouts([&computerName](const TimeoutKey &key) { return key == TimeoutKey{.category = ConnectionTimeouts::Invitation, .identifier = computerName}; });

	// send acceptance
	notifyStatus(ConnectionStatusUpdate::Type::Accepted, "Invitation from " + computerName + " accepted");

	if (!answerInvitation(computerName, true))
	{
		NETLINK_LOG_ERROR("Failed to send acceptance!");
		notifyStatus(ConnectionStatusUpdate::Type::Failed, "Failed to send connection acceptance", false);
		clearCurrentConnection();
		return false;
	}

	// The answer is queued before the ready flag, and the channel delivers both in order
	if (!openSession())
	{
		NETLINK_LOG_ERROR("Failed to open the session with {}", computerName);
		notifyStatus(ConnectionStatusUpdate::Type::Failed, "Failed to open the session", false);
		clearCurrentConnection();
		return false;
	}

	return true;
}


bool netlink::ConnectionService::declineIncomingConnection(const std::string &computerName, const std::string &reason)
{
	std::lock_guard<std::mutex> lock(mConnectingMutex);

	if (!mCurrentRequest.has_value() || mCurrentRequest->state != ConnectionStateInternal::InvitationReceived)
	{
		NETLINK_LOG_WARNING("No incoming invitation to decline");
		return false;
	}

	NETLINK_LOG_INFO("Declining connection from {}: {}", computerName, reason.empty() ? "No reason given." : reason);

	if (const bool result = answerInvitation(computerName, false, reason); !result)
		NETLINK_LOG_ERROR("Connection decline could not be sent!");

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

	// validate remote's name unless empty (always force close if left empty)
	if (!computerName.empty() && mCurrentRequest->remote.displayName != computerName)
	{
		NETLINK_LOG_WARNING("Connection close missmatch: request: {}, current: {}", computerName, mCurrentRequest->remote.displayName);
		return false;
	}

	// cancel all timeouts for this connection
	if (!remote.empty())
		disarmTimeouts([&remote](const TimeoutKey &key) { return key.identifier == remote; });
	else
		disarmAllTimeouts();

	// only proceed if we have an actual connection in progress or established
	if (!isConnected() && !isConnecting())
	{
		NETLINK_LOG_DEBUG("Connection already idle, clearing state");
		clearCurrentConnection();
		return true;
	}

	NETLINK_LOG_INFO("Closing current connection to {}", remote);

	mCurrentRequest->state = ConnectionStateInternal::Disconnecting;

	notifyStatus(ConnectionStatusUpdate::Type::Closing, "Closing connection");

	if (const bool result = sendDisconnectMessage(remote); !result)
		NETLINK_LOG_DEBUG("Connection disconnect message could no t be sent!");

	notifyStatus(ConnectionStatusUpdate::Type::Closed, "Connection closed");
	clearCurrentConnection();
	return true;
}


bool netlink::ConnectionService::hasIncomingInvitation() const
{
	std::lock_guard<std::mutex> lock(mConnectingMutex);

	return mCurrentRequest.has_value() && mCurrentRequest->state == ConnectionStateInternal::InvitationReceived;
}


std::optional<DiscoveryEndpoint> netlink::ConnectionService::getCurrentRemote() const
{
	std::lock_guard<std::mutex> lock(mConnectingMutex);

	if (mCurrentRequest.has_value())
		return mCurrentRequest->remote;

	return std::nullopt;
}


netlink::ConnectionStateInternal netlink::ConnectionService::getConnectionState() const
{
	std::lock_guard<std::mutex> lock(mConnectingMutex);

	if (mCurrentRequest.has_value())
		return mCurrentRequest->state;

	return ConnectionStateInternal::Idle;
}


bool netlink::ConnectionService::sendConnectionInvitation(const std::string &computerName) const
{
	if (const auto result = mValidatedPeers.get(computerName); !result.has_value())
	{
		NETLINK_LOG_ERROR("No valid result for {}", computerName);
		return false;
	}

	NETLINK_LOG_DEBUG("Sending connect request to {}", computerName);

	return mChannel.sendConnectRequest(computerName);
}


bool netlink::ConnectionService::sendDisconnectMessage(const std::string &computerName) const
{
	if (const auto result = mValidatedPeers.get(computerName); !result.has_value())
	{
		NETLINK_LOG_WARNING("sendDisconnectMessage: no validation result for {}", computerName);
		return false;
	}

	return mChannel.sendDisconnect(computerName);
}


bool netlink::ConnectionService::answerInvitation(const std::string &computerName, const bool connectionAccepted, const std::string &reason) const
{
	if (const auto result = mValidatedPeers.get(computerName); !result.has_value())
	{
		NETLINK_LOG_ERROR("answerInvitation: no validation result for {}", computerName);
		return false;
	}

	return mChannel.sendConnectAnswer(computerName, connectionAccepted, reason);
}


bool netlink::ConnectionService::sendConnectionReadyFlag(const std::string &computerName, const bool flag) const
{
	if (const auto result = mValidatedPeers.get(computerName); !result.has_value())
	{
		NETLINK_LOG_ERROR("sendConnectionReadyFlag: no validation result for {}", computerName);
		return false;
	}

	return mChannel.sendReadyFlag(computerName, flag);
}


void netlink::ConnectionService::onReceivedInvitation(const std::string &computerName)
{
	std::lock_guard<std::mutex> lock(mConnectingMutex);

	if (isConnected() || isConnecting())
	{
		NETLINK_LOG_WARNING("Received invitation from {} but already busy. We are declining..", computerName);
		if (const bool result = answerInvitation(computerName, false, "Already in a connection"); !result)
			NETLINK_LOG_ERROR("Could not decline invite!");
		return;
	}

	const auto validationResult = mValidatedPeers.get(computerName);
	if (!validationResult.has_value() || !validationResult->canConnect)
	{
		NETLINK_LOG_WARNING("Received invitation from unvalidated peer {}. Declining..", computerName);
		if (const bool result = answerInvitation(computerName, false, "Peer not validated"); !result)
			NETLINK_LOG_ERROR("Could not decline invite!");
		return;
	}

	NETLINK_LOG_INFO("Received connection invitation from {}", computerName);

	ConnectionRequest request;
	request.remote			 = validationResult->remoteEndpoint;
	request.validationResult = validationResult.value();
	request.requestTime		 = std::chrono::steady_clock::now();
	request.lastActivityTime = request.requestTime;
	request.isInitiator		 = false;
	request.state			 = ConnectionStateInternal::InvitationReceived;

	mCurrentRequest			 = std::move(request);
	mConnecting.store(true);

	if (mConfig.autoAcceptConnection)
	{
		NETLINK_LOG_INFO("Auto-accepting connection from {}", computerName);
		mTaskQueue.post([this, computerName]() { acceptIncomingConnection(computerName); });
		return;
	}

	// Ask the app (exactly once) and start a timeout in case it never responds
	notifyStatus(ConnectionStatusUpdate::Type::InvitationReceived, "Invitation from " + computerName);

	armTimeout({.category = ConnectionTimeouts::Invitation, .identifier = computerName}, mConfig.invitationTimeoutMs);
}


void netlink::ConnectionService::onReceivedAnswerToInvite(const std::string &computerName, const bool connectionAccepted, const std::string &reason)
{
	std::lock_guard<std::mutex> lock(mConnectingMutex);

	if (!mCurrentRequest.has_value())
	{
		NETLINK_LOG_WARNING("Received acceptance but no connection request exists!");
		return;
	}

	if (mCurrentRequest->state != ConnectionStateInternal::InvitationSent)
	{
		NETLINK_LOG_WARNING("Received acceptance in unexpected state: {}", static_cast<int>(mCurrentRequest->state));
		return;
	}

	// cancel invitation timeout
	disarmTimeouts([&computerName](const TimeoutKey &key) { return key == TimeoutKey{.category = ConnectionTimeouts::Invitation, .identifier = computerName}; });

	if (connectionAccepted)
	{
		NETLINK_LOG_INFO("Connection accepted by {}", computerName);

		mCurrentRequest->state			  = ConnectionStateInternal::Accepted;
		mCurrentRequest->lastActivityTime = std::chrono::steady_clock::now();

		notifyStatus(ConnectionStatusUpdate::Type::Accepted, "Connection accepted by " + computerName);

		if (!openSession())
		{
			NETLINK_LOG_ERROR("Failed to open the session with {}", computerName);
			notifyStatus(ConnectionStatusUpdate::Type::Failed, "Failed to open the session", false);
			clearCurrentConnection();
		}
	}
	else
	{
		NETLINK_LOG_WARNING("Connection declined by {}: {}", computerName, reason.empty() ? "no reason given" : reason);

		// The remote's reason reaches the app through ConnectionEvent::errorMessage
		const std::string detail = reason.empty() ? std::string{} : ": " + reason;
		notifyStatus(ConnectionStatusUpdate::Type::Declined, "Connection declined by " + computerName + detail, false);
		clearCurrentConnection();
	}
}


void netlink::ConnectionService::onReceivedConnectionReadyFlag(const std::string &computerName)
{
	// Caller must hold mConnectingMutex

	if (!mCurrentRequest.has_value() || mCurrentRequest->remote.displayName != computerName)
	{
		NETLINK_LOG_WARNING("Received ready flag from {} but no matching request", computerName);
		return;
	}

	NETLINK_LOG_INFO("Received ready flag from {}", computerName);

	mReadySync.setRemoteReady();
	mCurrentRequest->remoteReadyFlag = true;

	// Arriving before our own session opened, it completes in openSession()
	completeSessionIfReady();
}


void netlink::ConnectionService::onPeerValidated(const ValidationResult &peerValidation)
{
	NETLINK_LOG_INFO("Peer {} validated", peerValidation.remoteEndpoint.displayName);
	mValidatedPeers.store(peerValidation.remoteEndpoint.displayName, peerValidation);
}


void netlink::ConnectionService::clearCurrentConnection()
{
	NETLINK_LOG_INFO("Clearing current connection..");

	mCurrentRequest.reset();
	mConnected.store(false);
	mConnecting.store(false);
	disarmAllTimeouts();
	mReadySync.reset();
}


void netlink::ConnectionService::notifyStatus(const ConnectionStatusUpdate::Type type, const std::string &message, const bool success) const
{
	ConnectionStatusUpdate update;
	update.type	   = type;
	update.message = message;
	update.success = success;
	notifyStatus(std::move(update));
}


void netlink::ConnectionService::notifyStatus(ConnectionStatusUpdate update) const
{
	update.timestamp = std::chrono::steady_clock::now();

	// Every update names the peer it is about (callers notify before clearing the current request)
	if (update.endpoint.isEmpty() && mCurrentRequest.has_value())
		update.endpoint = mCurrentRequest->remote;

	if (update.success)
		NETLINK_LOG_INFO("Connection [{}]: {}", update.getTypeString(), update.message);
	else
		NETLINK_LOG_WARNING("Connection [{}] failed: {}", update.getTypeString(), update.message);

	if (mCallbacks.onStatusUpdate)
		mCallbacks.onStatusUpdate(update);
}


bool netlink::ConnectionService::openSession()
{
	// Caller must hold mConnectingMutex

	if (!mCurrentRequest.has_value())
		return false;

	const std::string remote = mCurrentRequest->remote.displayName;

	mCurrentRequest->state	 = ConnectionStateInternal::AwaitingReadyFlag;
	notifyStatus(ConnectionStatusUpdate::Type::Establishing, "Establishing session with " + remote);

	if (!sendConnectionReadyFlag(remote, true))
		return false;

	mReadySync.setLocalReady();
	mCurrentRequest->localReadyFlag = true;

	armTimeout({.category = ConnectionTimeouts::ReadyFlag, .identifier = remote}, mConfig.readyFlagTimeoutMs);

	completeSessionIfReady();
	return true;
}


void netlink::ConnectionService::completeSessionIfReady()
{
	// Caller must hold mConnectingMutex

	if (!mCurrentRequest.has_value() || mConnected.load() || mCurrentRequest->state != ConnectionStateInternal::AwaitingReadyFlag || !mReadySync.bothReady())
		return;

	NETLINK_LOG_INFO("Session with {} established", mCurrentRequest->remote.displayName);

	mCurrentRequest->state = ConnectionStateInternal::Connected;
	mConnected.store(true);
	mConnecting.store(false);

	disarmAllTimeouts();

	notifyStatus(ConnectionStatusUpdate::Type::Established, "Connected to " + mCurrentRequest->remote.displayName);
}


void netlink::ConnectionService::onPeerLost(const std::string &computerName, const std::string &reason)
{
	mTaskQueue.post(
		[this, computerName, reason]()
		{
			std::lock_guard<std::mutex> lock(mConnectingMutex);

			if (!mCurrentRequest.has_value() || mCurrentRequest->remote.displayName != computerName)
				return;

			NETLINK_LOG_WARNING("Lost {}: {}", computerName, reason);

			if (mConnected.load())
				notifyStatus(ConnectionStatusUpdate::Type::Closed, "Connection lost: " + reason, false);
			else
				notifyStatus(ConnectionStatusUpdate::Type::Failed, "Connection to " + computerName + " failed: " + reason, false);

			clearCurrentConnection();
		});
}


void netlink::ConnectionService::armTimeout(const TimeoutKey &key, const int timeoutMs)
{
	// Caller must hold mConnectingMutex
	const uint64_t generation = ++mTimeoutGeneration;
	mArmedTimeouts[key]		  = generation;

	// The timeout thread only posts: handling needs mConnectingMutex, and cancelling happens while holding it
	mTimeoutService.startTimeout(key, timeoutMs,
								 [this, generation](const TimeoutKey &expired)
								 {
									 mTaskQueue.post(
										 [this, expired, generation]()
										 {
											 std::lock_guard<std::mutex> lock(mConnectingMutex);

											 // Ignore a timeout that was cancelled or restarted while this task was queued
											 const auto					 it = mArmedTimeouts.find(expired);
											 if (it == mArmedTimeouts.end() || it->second != generation)
												 return;

											 mArmedTimeouts.erase(it);
											 onTimeout(expired);
										 });
								 });
}


void netlink::ConnectionService::disarmTimeouts(const std::function<bool(const TimeoutKey &)> &matches)
{
	// Caller must hold mConnectingMutex
	for (auto it = mArmedTimeouts.begin(); it != mArmedTimeouts.end();)
	{
		if (matches(it->first))
		{
			mTimeoutService.cancelTimeout(it->first);
			it = mArmedTimeouts.erase(it);
		}
		else
			++it;
	}
}


void netlink::ConnectionService::disarmAllTimeouts()
{
	// Caller must hold mConnectingMutex
	mArmedTimeouts.clear();
	mTimeoutService.cancelAll();
}


void netlink::ConnectionService::onTimeout(const TimeoutKey &key)
{
	// Caller must hold mConnectingMutex

	if (key.category == ConnectionTimeouts::Invitation)
	{
		NETLINK_LOG_WARNING("Invitation timed out for {}", key.identifier);
		notifyStatus(ConnectionStatusUpdate::Type::Failed, "Invitation timed out for " + key.identifier, false);
		clearCurrentConnection();
	}
	else if (key.category == ConnectionTimeouts::ReadyFlag)
	{
		NETLINK_LOG_WARNING("Ready flag timed out for {}", key.identifier);
		notifyStatus(ConnectionStatusUpdate::Type::Failed, "Remote " + key.identifier + " did not get ready", false);
		clearCurrentConnection();
	}
}
