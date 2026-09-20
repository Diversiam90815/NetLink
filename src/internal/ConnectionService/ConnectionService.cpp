/*
  ==============================================================================
	Module:         ConnectionService
	Description:    Orchestrates the full connection lifecycle with per-state
					timeouts, ReadyFlag synchronization, and peer validation.
  ==============================================================================
*/

#include "ConnectionService.h"
#include "NetLinkLog.h"


netlink::ConnectionService::ConnectionService(SignalingService &signaling, ITransportFactory &transportFactory) : mSignaling(signaling), mTransportFactory(&transportFactory)
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


void netlink::ConnectionService::onDataPortReceived(const std::string &computerName, int dataPort)
{
	std::lock_guard<std::mutex> lock(mConnectingMutex);

	if (!mCurrentRequest.has_value() || mCurrentRequest->remote.displayName != computerName)
		return;

	NETLINK_LOG_INFO("Received data port {} from {}", dataPort, computerName);
	mCurrentRequest->dataPort = dataPort;
}


netlink::ConnectionService::~ConnectionService()
{
	{
		// Tear down timeouts and transports first: their threads post into the task queue
		std::lock_guard<std::mutex> lock(mConnectingMutex);
		disarmAllTimeouts();
		mCurrentRequest.reset();
	}

	mTaskQueue.stop();
}


void netlink::ConnectionService::setConfig(const ConnectionConfig &config)
{
	mConfig = config;
	mRetryPolicy.setMaxRetries(mConfig.maxConnectionRetries);
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
	auto validationResult = mValidatedPeers.get(computerName);
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
	mRetryPolicy.reset();

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
	armTimeout({ConnectionTimeouts::Invitation, computerName}, mConfig.invitationTimeoutMs);

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

	// Start connection timeout
	armTimeout({ConnectionTimeouts::Connection, computerName}, mConfig.connectionTimeoutMs);

	// send acceptance
	notifyStatus(ConnectionStatusUpdate::Type::Accepted, "Invitation from " + computerName + " accepted");

	if (!answerInvitation(computerName, true))
	{
		NETLINK_LOG_ERROR("Failed to send acceptance!");
		notifyStatus(ConnectionStatusUpdate::Type::Failed, "Failed to send connection acceptance", false);
		clearCurrentConnection();
		return false;
	}

	mCurrentRequest->state = ConnectionStateInternal::EstablishingTransport;

	if (!determineLocalSessionRole())
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

	if (!mCurrentRequest.has_value() || mCurrentRequest->state != ConnectionStateInternal::InvitationReceived)
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

	sendDisconnectMessage(remote);

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


bool netlink::ConnectionService::sendConnectionInvitation(const std::string &computerName)
{
	auto result = mValidatedPeers.get(computerName);

	if (!result.has_value())
	{
		NETLINK_LOG_ERROR("No valid result for {}", computerName);
		return false;
	}

	NETLINK_LOG_DEBUG("Sending connect request to {}", computerName);

	mSignaling.sendConnectRequest(computerName);

	return true;
}


bool netlink::ConnectionService::sendDisconnectMessage(const std::string &computerName)
{
	auto result = mValidatedPeers.get(computerName);

	if (!result.has_value())
	{
		NETLINK_LOG_WARNING("sendDisconnectMessage: no validation result for {}", computerName);
		return false;
	}

	mSignaling.sendDisconnect(computerName);

	return true;
}


bool netlink::ConnectionService::answerInvitation(const std::string &computerName, const bool connectionAccepted, const std::string &reason)
{
	auto result = mValidatedPeers.get(computerName);

	if (!result.has_value())
	{
		NETLINK_LOG_ERROR("answerInvitation: no validation result for {}", computerName);
		return false;
	}

	mSignaling.sendConnectAnswer(computerName, connectionAccepted, reason);

	return true;
}


bool netlink::ConnectionService::sendConnectionReadyFlag(const std::string &computerName, const bool flag)
{
	auto result = mValidatedPeers.get(computerName);

	if (!result.has_value())
	{
		NETLINK_LOG_ERROR("sendConnectionReadyFlag: no validation result for {}", computerName);
		return false;
	}

	mSignaling.sendReadyFlag(computerName, flag);

	return true;
}


void netlink::ConnectionService::onReceivedInvitation(const std::string &computerName)
{
	std::lock_guard<std::mutex> lock(mConnectingMutex);

	if (isConnected() || isConnecting())
	{
		NETLINK_LOG_WARNING("Received invitation from {} but already busy. We are declining..", computerName);
		answerInvitation(computerName, false, "Already in a connection");
		return;
	}

	auto validationResult = mValidatedPeers.get(computerName);
	if (!validationResult.has_value() || !validationResult->canConnect)
	{
		NETLINK_LOG_WARNING("Received invitation from unvalidated peer {}. Declining..", computerName);
		answerInvitation(computerName, false, "Peer not validated");
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

	armTimeout({ConnectionTimeouts::Invitation, computerName}, mConfig.invitationTimeoutMs);
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
	disarmTimeouts([&computerName](const TimeoutKey &key) { return key == TimeoutKey{ConnectionTimeouts::Invitation, computerName}; });

	if (connectionAccepted)
	{
		NETLINK_LOG_INFO("Connection accepted by {}", computerName);

		mCurrentRequest->state			  = ConnectionStateInternal::Accepted;
		mCurrentRequest->lastActivityTime = std::chrono::steady_clock::now();

		notifyStatus(ConnectionStatusUpdate::Type::Accepted, "Connection accepted by " + computerName);

		// start connection timeout
		armTimeout({ConnectionTimeouts::Connection, computerName}, mConfig.connectionTimeoutMs);

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


void netlink::ConnectionService::onReceivedConnectionReadyFlag(const std::string &computerName)
{
	// Caller must hold mConnectingMutex

	if (!mCurrentRequest.has_value())
	{
		NETLINK_LOG_WARNING("Received ready flag but no active request");
		return;
	}

	NETLINK_LOG_INFO("Received ready flag from {}", computerName);

	disarmTimeouts([&computerName](const TimeoutKey &key) { return key == TimeoutKey{ConnectionTimeouts::ReadyFlag, computerName}; });
	mReadySync.setRemoteReady();

	// If we are the Connector, the Acceptor's data port arrived before its ready flag
	if (mCurrentRequest->localRole == SessionRole::Connector && mCurrentRequest->client)
	{
		if (mCurrentRequest->dataPort == 0)
		{
			NETLINK_LOG_ERROR("Connector: ready flag from {} without data port", computerName);
			return;
		}

		NETLINK_LOG_INFO("Connector: connecting to {}:{}", mCurrentRequest->remote.IPAddress, mCurrentRequest->dataPort);
		mCurrentRequest->state = ConnectionStateInternal::Connected;
		mCurrentRequest->client->connect(mLocalIP, mCurrentRequest->remote.IPAddress, static_cast<unsigned short>(mCurrentRequest->dataPort));
	}
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
	mRetryPolicy.reset();
	disarmAllTimeouts();
	mReadySync.reset();
}


bool netlink::ConnectionService::retryConnection()
{
	if (!mCurrentRequest.has_value())
		return false;

	if (!mRetryPolicy.recordAttempt())
	{
		NETLINK_LOG_WARNING("Max connection retried ({}) reached!", mConfig.maxConnectionRetries);

		notifyStatus(ConnectionStatusUpdate::Type::Failed, "Max retries reached", false);
		return false;
	}

	NETLINK_LOG_INFO("Retrying connection (attempt {}/{})", mRetryPolicy.attempts(), mConfig.maxConnectionRetries);

	// Re-establish transport layer
	mCurrentRequest->server.reset();
	mCurrentRequest->client.reset();
	mCurrentRequest->state = ConnectionStateInternal::EstablishingTransport;
	mReadySync.reset();

	return determineLocalSessionRole();
}


void netlink::ConnectionService::notifyStatus(ConnectionStatusUpdate::Type type, const std::string &message, bool success)
{
	ConnectionStatusUpdate update;
	update.type	   = type;
	update.message = message;
	update.success = success;
	notifyStatus(std::move(update));
}


void netlink::ConnectionService::notifyStatus(ConnectionStatusUpdate update)
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


bool netlink::ConnectionService::determineLocalSessionRole()
{
	// Caller must hold mutex

	if (!mCurrentRequest.has_value())
		return false;

	const net::IPv4Address &remoteIP = mCurrentRequest->remote.IPAddress;
	SessionRole				role	 = determineRole(mLocalIP, remoteIP);
	mCurrentRequest->localRole		 = role;

	NETLINK_LOG_INFO("Session role for {}: {}", remoteIP.toString(), role == SessionRole::Acceptor ? "Acceptor" : "Connector");

	if (role == SessionRole::Acceptor)
	{
		auto server = mTransportFactory->createServer();

		// Transport callbacks run on transport threads
		server->setSessionHandler([this](ISession::pointer session) { mTaskQueue.post([this, session]() { onTransportEstablished(session); }); });

		if (!server->start(mLocalIP))
		{
			NETLINK_LOG_ERROR("Acceptor: failed to listen on {}", mLocalIP.toString());
			return false;
		}

		int boundPort = server->getBoundPort();
		NETLINK_LOG_INFO("Acceptor: Listening on port {}", boundPort);

		mCurrentRequest->server = std::move(server);

		// communicate bound port
		mSignaling.sendDataPort(mCurrentRequest->remote.displayName, boundPort);

		sendConnectionReadyFlag(mCurrentRequest->remote.displayName, true);

		armTimeout({ConnectionTimeouts::ReadyFlag, mCurrentRequest->remote.displayName}, mConfig.readyFlagTimeoutMs);
	}
	else if (role == SessionRole::Connector)
	{
		auto client = mTransportFactory->createClient();

		client->setConnectHandler([this](ISession::pointer session) { mTaskQueue.post([this, session]() { onTransportEstablished(session); }); });

		client->setConnectTimeoutHandler(
			[this]()
			{
				NETLINK_LOG_ERROR("TCP connection attempt timed out or was refused");
				mTaskQueue.post(
					[this]()
					{
						std::lock_guard<std::mutex> lock(mConnectingMutex);
						notifyStatus(ConnectionStatusUpdate::Type::Failed, "TCP connection timed out or refused", false);
						clearCurrentConnection();
					});
			});

		mCurrentRequest->client = std::move(client);

		sendConnectionReadyFlag(mCurrentRequest->remote.displayName, true);

		// If the acceptor's dataport + readyflag already arrived before we got here, connect now
		if (mReadySync.isRemoteReady() && mCurrentRequest->dataPort != 0)
		{
			NETLINK_LOG_INFO("Connector: remote already ready, connecting immediately to {}:{}", mCurrentRequest->remote.IPAddress, mCurrentRequest->dataPort);
			mCurrentRequest->state = ConnectionStateInternal::Connected;
			mCurrentRequest->client->connect(mLocalIP, mCurrentRequest->remote.IPAddress, static_cast<unsigned short>(mCurrentRequest->dataPort));
		}
		else
		{
			// Wait for ReadyFlag from Acceptor (handled in onReceivedConnectionReadyFlag)
			armTimeout({ConnectionTimeouts::ReadyFlag, mCurrentRequest->remote.displayName}, mConfig.readyFlagTimeoutMs);
		}
	}
	else
	{
		NETLINK_LOG_WARNING("Unknwon session role determined..");
		return false;
	}

	return true;
}


void netlink::ConnectionService::onTransportEstablished(const ISession::pointer &session)
{
	std::lock_guard<std::mutex> lock(mConnectingMutex);

	if (!session)
		return;

	if (!mCurrentRequest.has_value() || mConnected.load())
	{
		NETLINK_LOG_WARNING("Transport: dropping session from {}, no connection expected", session->getRemoteAddress());
		session->close();
		return;
	}

	// Anybody on the network can connect to the listening port: only accept the peer we negotiated with
	if (session->getRemoteAddress() != mCurrentRequest->remote.IPAddress)
	{
		NETLINK_LOG_WARNING("Transport: rejecting session from {}, expected {}", session->getRemoteAddress(), mCurrentRequest->remote.IPAddress);
		session->close();
		return;
	}

	NETLINK_LOG_INFO("Transport: session with {} established", session->getRemoteAddress());

	// One peer per connection: stop accepting further inbound connections
	if (mCurrentRequest->server)
		mCurrentRequest->server->stop();

	mCurrentRequest->session = session;
	mCurrentRequest->state	 = ConnectionStateInternal::Connected;
	mReadySync.setLocalReady();
	mConnected.store(true);
	mConnecting.store(false);

	disarmAllTimeouts();

	ConnectionStatusUpdate update;
	update.type	   = ConnectionStatusUpdate::Type::Established;
	update.session = session;
	update.success = true;
	notifyStatus(std::move(update));
}


void netlink::ConnectionService::onTransportDisconnected(const std::string &reason)
{
	mTaskQueue.post(
		[this, reason]()
		{
			std::lock_guard<std::mutex> lock(mConnectingMutex);

			if (!mConnected.load())
				return;

			NETLINK_LOG_WARNING("Transport: connection lost: {}", reason);

			notifyStatus(ConnectionStatusUpdate::Type::Closed, "Connection lost: " + reason, false);
			clearCurrentConnection();
		});
}


void netlink::ConnectionService::setTransportFactory(ITransportFactory &transportFactory)
{
	std::lock_guard<std::mutex> lock(mConnectingMutex);
	mTransportFactory = &transportFactory;
}


void netlink::ConnectionService::armTimeout(const TimeoutKey &key, int timeoutMs)
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
											 auto						 it = mArmedTimeouts.find(expired);
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

	if (key.category == ConnectionTimeouts::Connection)
	{
		NETLINK_LOG_WARNING("Connection timed out for {}", key.identifier);
		notifyStatus(ConnectionStatusUpdate::Type::Failed, "Connection timed out for " + key.identifier, false);
		clearCurrentConnection();
	}
	else if (key.category == ConnectionTimeouts::Invitation)
	{
		NETLINK_LOG_WARNING("Invitation timed out for {}", key.identifier);
		notifyStatus(ConnectionStatusUpdate::Type::Failed, "Invitation timed out for " + key.identifier, false);
		clearCurrentConnection();
	}
	else if (key.category == ConnectionTimeouts::ReadyFlag)
	{
		NETLINK_LOG_WARNING("Ready flag timed out for {}", key.identifier);
		if (!retryConnection())
		{
			notifyStatus(ConnectionStatusUpdate::Type::Failed, "Remote " + key.identifier + " did not get ready", false);
			clearCurrentConnection();
		}
	}
}
