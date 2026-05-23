/*
  ==============================================================================
	Module:         RemoteCommunication
	Description:    Managing the messages themselves, sending and receiving
  ==============================================================================
*/

#include "RemoteCommunication.h"
#include "NetLinkLog.h"


bool RemoteCommunication::init(std::shared_ptr<ISession> session, const std::string &secret)
{
	if (mIsInitialized.load())
	{
		NETLINK_LOG_ERROR("Already initialized. Call deinit() first.");
		return false;
	}

	if (!session)
	{
		NETLINK_LOG_ERROR("Session is not valid. We received a nullptr. Cannot initialize");
		return false;
	}
	else if (!session->isConnected())
	{
		NETLINK_LOG_ERROR("Tried to init with a non-connected session.");
		return false;
	}

	mSession = session;
	mSecret		= secret;

	if (mSendThread)
		mSendThread->stop();

	if (mReceiveThread)
		mReceiveThread->stop();

	mSendThread.reset(new SendThread(this));
	mReceiveThread.reset(new ReceiveThread(this));

	mIsInitialized.store(true);
	return true;
}


void RemoteCommunication::deinit()
{
	if (mSendThread)
		mSendThread->stop();

	if (mReceiveThread)
		mReceiveThread->stop();

	// Try to send any remaining critical messages (like disconnect)
	if (mSession && mSession->isConnected())
	{
		// Send remaining outgoing messages with a timeout
		auto timeout = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
		while (!mOutgoingMessages.empty() && std::chrono::steady_clock::now() < timeout)
		{
			if (!sendMessages())
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}

	if (mSession)
	{
		mSession->stopReadAsync();
		mSession.reset();
		mSession = nullptr;
	}

	clearPendingMessages();
	mIsInitialized.store(false);
}


void RemoteCommunication::start()
{
	if (!isInitialized())
		return;

	// Start async read
	mSession->startReadAsync(
		[this](netlink::InternalMessage message)
		{
			const size_t secretLen = mSecret.size();

			// Validate and process the received message body
			if (message.data.size() < secretLen)
			{
				NETLINK_LOG_ERROR("Received message is too small to contain secret identifier.");
				return;
			}

			if (memcmp(message.data.data(), mSecret.data(), secretLen) != 0)
			{
				NETLINK_LOG_ERROR("Secret mismatch — rejecting message.");
				return;
			}

			// The secret is valid. Strip it from the data.
			message.data.erase(message.data.begin(), message.data.begin() + secretLen);

			// Queue the message
			{
				std::lock_guard<std::mutex> lock(mIncomingListMutex);
				mIncomingMessages.push_back(message);
			};
		});

	mSendThread->start();
	mReceiveThread->start();
}


void RemoteCommunication::stop()
{
	if (mSendThread)
		mSendThread->stop();

	if (mReceiveThread)
		mReceiveThread->stop();
}


bool RemoteCommunication::read(uint32_t &type, std::vector<uint8_t> &dest)
{
	std::lock_guard<std::mutex> lock(mIncomingListMutex);

	if (mIncomingMessages.empty())
		return false;

	auto &front = mIncomingMessages.front();
	type		= front.type;
	dest		= std::move(front.data);
	mIncomingMessages.erase(mIncomingMessages.begin());
	return true;
}


void RemoteCommunication::write(uint32_t type, std::vector<uint8_t> data)
{
	if (!isInitialized())
		return;

	std::lock_guard<std::mutex> lock(mOutgoingListMutex);

	netlink::InternalMessage	message;
	message.type = type;

	// Prepend the secret to the message data
	message.data.reserve(mSecret.size() + data.size());
	message.data.insert(message.data.end(), mSecret.begin(), mSecret.end());
	message.data.insert(message.data.end(), data.begin(), data.end());

	mOutgoingMessages.push_back(message);
	mSendThread->triggerEvent();
}


void RemoteCommunication::clearPendingMessages()
{
	{
		std::lock_guard<std::mutex> lock(mIncomingListMutex);
		mIncomingMessages.clear();
	}

	{
		std::lock_guard<std::mutex> lock(mOutgoingListMutex);
		mOutgoingMessages.clear();
	}
}


bool RemoteCommunication::receiveMessages()
{
	if (!isInitialized())
		return false;

	// Get all messages from the queue
	std::vector<netlink::InternalMessage> messages;

	{
		std::lock_guard<std::mutex> lock(mIncomingListMutex);
		if (mIncomingMessages.empty())
			return true; // No messages, but not an error

		messages.swap(mIncomingMessages);
	}

	for (auto &msg : messages)
	{
		if (mCallback)
			mCallback(msg.type, msg.data);
	}

	return true;
}


bool RemoteCommunication::sendMessages()
{
	if (!isInitialized())
		return false;

	// Swap under lock to minimize lock duration
	std::vector<netlink::InternalMessage> toSend;
	{
		std::lock_guard<std::mutex> lock(mOutgoingListMutex);
		toSend.swap(mOutgoingMessages);
	}


    for (auto &message : toSend)
	{
		if (!mSession->sendMessage(message))
			return false;
	}

	return true;
}
