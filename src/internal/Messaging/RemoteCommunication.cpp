/*
  ==============================================================================
	Module:         RemoteCommunication
	Description:    Managing the messages themselves, sending and receiving
  ==============================================================================
*/

#include "RemoteCommunication.h"

#include "NetLinkLog.h"


std::shared_ptr<netlink::ISession> RemoteCommunication::session() const
{
	std::lock_guard<std::mutex> lock(mSessionMutex);
	return mSession;
}


std::shared_ptr<SendThread> RemoteCommunication::sendThread() const
{
	std::lock_guard<std::mutex> lock(mSessionMutex);
	return mSendThread;
}


std::shared_ptr<ReceiveThread> RemoteCommunication::receiveThread() const
{
	std::lock_guard<std::mutex> lock(mSessionMutex);
	return mReceiveThread;
}


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

	// stop() joins, so never call it while holding mSessionMutex
	if (auto previousSend = sendThread())
		previousSend->stop();

	if (auto previousReceive = receiveThread())
		previousReceive->stop();

	auto sendWorker	   = std::make_shared<SendThread>(this);
	auto receiveWorker = std::make_shared<ReceiveThread>(this);

	{
		std::lock_guard<std::mutex> lock(mSessionMutex);
		mSession	   = std::move(session);
		mSendThread	   = std::move(sendWorker);
		mReceiveThread = std::move(receiveWorker);
	}

	mIsInitialized.store(true);
	return true;
}


void RemoteCommunication::deinit()
{
	auto activeSession = session();
	auto sendWorker	   = sendThread();
	auto receiveWorker = receiveThread();

	// Stop inbound delivery first, the read callback wakes the receive thread
	if (activeSession)
		activeSession->stopReadAsync();

	if (sendWorker)
		sendWorker->stop();

	if (receiveWorker)
		receiveWorker->stop();

	// Try to send any remaining critical messages
	if (activeSession && activeSession->isConnected())
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

	{
		std::lock_guard<std::mutex> lock(mSessionMutex);
		mSession.reset();
	}

	clearPendingMessages();
	mIsInitialized.store(false);
}


void RemoteCommunication::start()
{
	if (!isInitialized())
		return;

	auto activeSession = session();
	auto sendWorker	   = sendThread();
	auto receiveWorker = receiveThread();

	if (!activeSession || !sendWorker || !receiveWorker)
		return;

	activeSession->startReadAsync(
		[this](netlink::InternalMessage message)
		{
			{
				std::lock_guard<std::mutex> lock(mIncomingListMutex);
				mIncomingMessages.push_back(std::move(message));
			}
			if (auto worker = receiveThread())
				worker->triggerEvent();
		},
		[this](const std::string &reason)
		{
			if (mDisconnectedCallback)
				mDisconnectedCallback(reason);
		});

	sendWorker->start();
	receiveWorker->start();
}


void RemoteCommunication::stop()
{
	if (auto sendWorker = sendThread())
		sendWorker->stop();

	if (auto receiveWorker = receiveThread())
		receiveWorker->stop();
}


void RemoteCommunication::write(uint32_t type, std::vector<uint8_t> data, netlink::DeliveryMode mode)
{
	if (!isInitialized())
		return;

	{
		std::lock_guard<std::mutex> lock(mOutgoingListMutex);
		mOutgoingMessages.push_back({netlink::InternalMessage{type, std::move(data)}, mode});
	}

	if (auto sendWorker = sendThread())
		sendWorker->triggerEvent();
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
	auto activeSession = session();

	if (!isInitialized() || !activeSession)
		return false;

	// Swap under lock to minimize lock duration
	std::vector<netlink::OutgoingMessage> toSend;
	{
		std::lock_guard<std::mutex> lock(mOutgoingListMutex);
		toSend.swap(mOutgoingMessages);
	}

	for (size_t index = 0; index < toSend.size(); ++index)
	{
		if (activeSession->sendMessage(toSend[index].message, toSend[index].mode))
			continue;
		const size_t unsent = toSend.size() - index;

		{
			std::lock_guard<std::mutex> lock(mOutgoingListMutex);
			mOutgoingMessages.insert(mOutgoingMessages.begin(), std::make_move_iterator(toSend.begin() + static_cast<std::ptrdiff_t>(index)),
									 std::make_move_iterator(toSend.end()));
		}

		NETLINK_LOG_WARNING("Sending failed, {} message(s) requeued", unsent);
		return false;
	}

	return true;
}
