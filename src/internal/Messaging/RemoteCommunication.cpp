/*
  ==============================================================================
	Module:         RemoteCommunication
	Description:    Managing the messages themselves, sending and receiving
  ==============================================================================
*/

#include "RemoteCommunication.h"
#include "NetLinkLog.h"


bool RemoteCommunication::init(std::shared_ptr<netlink::ISession> session)
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

	if (mSendThread)
		mSendThread->stop();

	if (mReceiveThread)
		mReceiveThread->stop();

	mSendThread	   = std::make_shared<SendThread>(this);
	mReceiveThread = std::make_shared<ReceiveThread>(this);

	mIsInitialized.store(true);
	return true;
}


void RemoteCommunication::deinit()
{
	// Stop inbound delivery first, the read callback wakes the receive thread
	if (mSession)
		mSession->stopReadAsync();

	if (mSendThread)
		mSendThread->stop();

	if (mReceiveThread)
		mReceiveThread->stop();

	// Try to send any remaining critical messages
	if (mSession && mSession->isConnected())
	{
		// Send remaining outgoing messages with a timeout
		auto timeout = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
		while (std::chrono::steady_clock::now() < timeout)
		{
			{
				std::lock_guard<std::mutex> lock(mOutgoingListMutex);
				if (mOutgoingMessages.empty())
					break;
			}

			if (!sendMessages())
				break;
		}
	}

	mSession.reset();

	clearPendingMessages();
	mIsInitialized.store(false);
}


void RemoteCommunication::start()
{
	if (!isInitialized())
		return;

	mSession->startReadAsync(
		[this](netlink::InternalMessage message)
		{
			{
				std::lock_guard<std::mutex> lock(mIncomingListMutex);
				mIncomingMessages.push_back(std::move(message));
			}
			mReceiveThread->triggerEvent();
		},
		[this](const std::string &reason)
		{
			if (mDisconnectedCallback)
				mDisconnectedCallback(reason);
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


void RemoteCommunication::write(uint32_t type, std::vector<uint8_t> data, netlink::DeliveryMode mode)
{
	if (!isInitialized())
		return;

	{
		std::lock_guard<std::mutex> lock(mOutgoingListMutex);
		mOutgoingMessages.push_back({netlink::InternalMessage{type, std::move(data)}, mode});
	}

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
	if (!isInitialized() || !mSession)
		return false;

	// Swap under lock to minimize lock duration
	std::vector<netlink::OutgoingMessage> toSend;
	{
		std::lock_guard<std::mutex> lock(mOutgoingListMutex);
		toSend.swap(mOutgoingMessages);
	}

	for (const auto &outgoing : toSend)
	{
		if (!mSession->sendMessage(outgoing.message, outgoing.mode))
			return false;
	}

	return true;
}
