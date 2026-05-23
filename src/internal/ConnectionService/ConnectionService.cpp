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
	mDeferredRunning.store(true);
	mDeferredThread = std::thread(
		[this]()
		{
			while (mDeferredRunning.load())
			{
				std::unique_lock<std::mutex> lock(mDeferredMutex);
				mDeferredCV.wait(lock, [this] { return !mDeferredQueue.empty() || !mDeferredRunning.load(); });

				while (!mDeferredQueue.empty())
				{
					auto fn = std::move(mDeferredQueue.front());
					mDeferredQueue.pop();
					lock.unlock();
					fn();
					lock.lock();
				}
			}
		});

	SignalingCallbacks cb;
	cb.onConnectRequested		= [this](const std::string &computerName) { onReceivedInvitation(computerName); };
	cb.onConnectRequestAnswered = [this](const std::string &computerName, bool accepted) { onReceivedAnswerToInvite(computerName, accepted, ""); };

	cb.onDisconnectReceived		= [this](const std::string &computerName)
	{
		dispatchDeferred(
			[this, computerName]()
			{
				NETLINK_LOG_INFO("Remote {} disconnected", computerName);

				clearCurrentConnection();

				if (mCallbacks.onDisconnected)
					mCallbacks.onDisconnected();
			});
	};

	cb.onReadyFlagReceived = [this](const std::string &computerName)
	{
		dispatchDeferred(
			[this, computerName]()
			{
				std::lock_guard<std::mutex> lock(mConnectingMutex);
				onReceivedConnectionReadyFlag(computerName);
			});
	};

	cb.onDataPortReceived = [this](const std::string &computerName, int dataPort)
	{
		std::lock_guard<std::mutex> lock(mConnectingMutex);
		if (!mCurrentRequest.has_value())
			return;

		NETLINK_LOG_INFO("Received data port {} from {}", dataPort, computerName);
		mCurrentRequest->remote.port = dataPort;
	};

	mSignaling.setCallbacks(std::move(cb));
}


netlink::ConnectionService::~ConnectionService()
{
	mDeferredRunning.store(false);

	{
		std::lock_guard<std::mutex> lock(mDeferredMutex);
		while (!mDeferredQueue.empty())
			mDeferredQueue.pop();
	}

	mDeferredCV.notify_all();

	if (mDeferredThread.joinable())
		mDeferredThread.join();
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
	request.state			 = ConnectionStateInternal::Initiated;

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
	mCurrentRequest->state = ConnectionStateInternal::InvitationSent;

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

	mCurrentRequest->state = ConnectionStateInternal::EstablishingTransport;

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

	mCurrentRequest->state = ConnectionStateInternal::Disconnecting;

	notifyStatus(ConnectionStatusUpdate::Type::Closing, "Closing connection");

	sendDisconnectMessage(computerName);

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
	auto result = findValidationResult(computerName);

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
	auto result = findValidationResult(computerName);

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
	auto result = findValidationResult(computerName);

	if (!result.has_value())
	{
		NETLINK_LOG_ERROR("answerInvitation: no validation result for {}", computerName);
		return false;
	}

	mSignaling.sendConnectAnswer(computerName, connectionAccepted);

	return true;
}


bool netlink::ConnectionService::sendConnectionReadyFlag(const std::string &computerName, const bool flag)
{
	auto result = findValidationResult(computerName);

	if (!result.has_value())
	{
		NETLINK_LOG_ERROR("sendConnectionReadyFlag: no validation result for {}", computerName);
		return false;
	}

	mSignaling.sendReadyFlag(computerName);

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

	auto validationResult = findValidationResult(computerName);
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

	notifyStatus(ConnectionStatusUpdate::Type::InvitationReceived, "Invitation from " + computerName);

	if (mConfig.autoAcceptConnection)
	{
		NETLINK_LOG_INFO("Auto-accepting connection from {}", computerName);
		dispatchDeferred([this, computerName]() { acceptIncomingConnection(computerName); });
		return;
	}

	// prompt the app to decide
	if (mCallbacks.onConnectionRequested)
		mCallbacks.onConnectionRequested(mCurrentRequest->remote);
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
	mTimeoutService.cancelTimeout({ConnectionTimeouts::Invitation, computerName});

	if (connectionAccepted)
	{
		NETLINK_LOG_INFO("Connection accepted by {}", computerName);

		mCurrentRequest->state			  = ConnectionStateInternal::Accepted;
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


void netlink::ConnectionService::onReceivedConnectionReadyFlag(const std::string &computerName)
{
	// Caller must hold mConnectingMutex

	if (!mCurrentRequest.has_value())
	{
		NETLINK_LOG_WARNING("Received ready flag but no active request");
		return;
	}

	NETLINK_LOG_INFO("Received ready flag from {}", computerName);

	mTimeoutService.cancelTimeout({ConnectionTimeouts::ReadyFlag, computerName});
	mRemoteReady.store(true);

	// If we are the Connector, we now know the Acceptor's bound port from the packet
	// (port was updated in the callback lambda before this call)
	if (mCurrentRequest->localRole == SessionRole::Connector && mCurrentRequest->client)
	{
		NETLINK_LOG_INFO("Connector: connecting to {}:{}", mCurrentRequest->remote.IPAddress, mCurrentRequest->remote.port);
		mCurrentRequest->state = ConnectionStateInternal::Connected;
		mCurrentRequest->client->connect(mCurrentRequest->remote.IPAddress, static_cast<unsigned short>(mCurrentRequest->remote.port));
	}
}


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
	if (!mCurrentRequest.has_value())
		return false;

	if (mConnectionAttempts >= mConfig.maxConnectionRetries)
	{
		NETLINK_LOG_WARNING("Max connection retried ({}) reached!", mConfig.maxConnectionRetries);

		notifyStatus(ConnectionStatusUpdate::Type::Failed, "Max retries reached", false);

		if (mCallbacks.onConnectionFailed)
			mCallbacks.onConnectionFailed("Max retries reached");

		return false;
	}

	++mConnectionAttempts;
	NETLINK_LOG_INFO("Retrying connection (attempt {}/{})", mConnectionAttempts, mConfig.maxConnectionRetries);

	std::string computerName = mCurrentRequest->remote.displayName;

	// Re-establish transport layer
	mCurrentRequest->server.reset();
	mCurrentRequest->client.reset();
	mCurrentRequest->state = ConnectionStateInternal::EstablishingTransport;
	mLocalReady.store(false);
	mRemoteReady.store(false);

	return determineLocalSessionRole();
}


void netlink::ConnectionService::notifyStatus(ConnectionStatusUpdate::Type type, const std::string &message, bool success) {}


bool netlink::ConnectionService::determineLocalSessionRole()
{
	// Caller must hold mutex

	if (!mCurrentRequest.has_value())
		return false;

	const std::string &remoteIP = mCurrentRequest->remote.IPAddress;
	SessionRole		   role		= determineRole(mLocalIP, remoteIP);
	mCurrentRequest->localRole	= role;

	NETLINK_LOG_INFO("Session role for {}: {}", remoteIP, role == SessionRole::Acceptor ? "Acceptor" : "Connector");

	if (role == SessionRole::Acceptor)
	{
		auto server = mTransportFactory.createServer(mIoContext);

		server->setSessionHandler(
			[this](ISession::pointer session)
			{
				NETLINK_LOG_INFO("Transport: incoming session accepted!");

				std::lock_guard<std::mutex> lock(mConnectingMutex);

				if (!mCurrentRequest.has_value())
					return;

				mCurrentRequest->session = session;
				mCurrentRequest->state	 = ConnectionStateInternal::Connected;
				mLocalReady.store(true);
				mConnected.store(true);
				mConnecting.store(false);

				mTimeoutService.cancelAll();

				if (mCallbacks.onConnected)
					mCallbacks.onConnected(session);
			});

		server->startAccept();

		int boundPort = server->getBoundPort();
		NETLINK_LOG_INFO("Acceptor: Listening on port {}", boundPort);

		mCurrentRequest->server = std::move(server);

		// communicate bound port
		mSignaling.sendDataPort(mCurrentRequest->remote.displayName, boundPort);

		sendConnectionReadyFlag(mCurrentRequest->remote.displayName, true);

		mTimeoutService.startTimeout({ConnectionTimeouts::ReadyFlag, mCurrentRequest->remote.displayName}, mConfig.readyFlagTimeoutMs,
									 [this](const TimeoutKey &key) { onTimeout(key); });
	}
	else if (role == SessionRole::Connector)
	{
		auto client = mTransportFactory.createClient(mIoContext);

		client->setConnectHandler(
			[this](ISession::pointer session)
			{
				NETLINK_LOG_INFO("Transport: outbound connection established!");

				std::lock_guard<std::mutex> lock(mConnectingMutex);

				if (!mCurrentRequest.has_value())
					return;

				mCurrentRequest->session = session;
				mCurrentRequest->state	 = ConnectionStateInternal::Connected;
				mLocalReady.store(true);
				mConnected.store(true);
				mConnecting.store(false);

				mTimeoutService.cancelAll();

				if (mCallbacks.onConnected)
					mCallbacks.onConnected(session);
			});

		mCurrentRequest->client = std::move(client);

		sendConnectionReadyFlag(mCurrentRequest->remote.displayName, true);

		// If the acceptor's dataport + readyflag already arrived before we got here, connect now
		if (mRemoteReady.load() && mCurrentRequest->remote.port != 0)
		{
			NETLINK_LOG_INFO("Connector: remote already ready, connecting immediately to {}:{}", mCurrentRequest->remote.IPAddress, mCurrentRequest->remote.port);
			mCurrentRequest->state = ConnectionStateInternal::Connected;
			mCurrentRequest->client->connect(mCurrentRequest->remote.IPAddress, static_cast<unsigned short>(mCurrentRequest->remote.port));
		}
		else
		{
			// Wait for ReadyFlag from Acceptor (handled in onReceivedConnectionReadyFlag)
			mTimeoutService.startTimeout({ConnectionTimeouts::ReadyFlag, mCurrentRequest->remote.displayName}, mConfig.readyFlagTimeoutMs,
										 [this](const TimeoutKey &key) { onTimeout(key); });
		}
	}
	else
	{
		NETLINK_LOG_WARNING("Unknwon session role determined..");
		return false;
	}
}


void netlink::ConnectionService::dispatchDeferred(std::function<void()> fn)
{
	{
		std::lock_guard<std::mutex> lock(mDeferredMutex);
		mDeferredQueue.push(std::move(fn));
	}
	mDeferredCV.notify_one();
}


void netlink::ConnectionService::onTimeout(const TimeoutKey &key)
{
	std::lock_guard<std::mutex> lock(mConnectingMutex);

	if (key.category == ConnectionTimeouts::Connection)
	{
		NETLINK_LOG_WARNING("Connection timed out for {}", key.identifier);
		notifyStatus(ConnectionStatusUpdate::Type::Failed, "Invitation timed out for " + key.identifier, false);
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
			clearCurrentConnection();
	}
}
