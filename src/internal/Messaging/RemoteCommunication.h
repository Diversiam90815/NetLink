/*
  ==============================================================================
	Module:         RemoteCommunication
	Description:    Managing the messages themselves, sending and receiving
  ==============================================================================
*/

#pragma once

#include <functional>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "Transport/TransportInterfaces.h"
#include "CommunicationThreads.h"
#include "MessageTypes.h"


class RemoteCommunication
{
public:
	using MessageCallback  = std::function<void(uint32_t type, std::vector<uint8_t> &data)>;

	RemoteCommunication()  = default;
	~RemoteCommunication() = default;

	bool init(std::shared_ptr<ISession> session);
	void deinit();

	void start();
	void stop();

	void write(uint32_t type, std::vector<uint8_t> data);
	void setMessageCallback(MessageCallback cb) { mCallback = cb; }

	bool isInitialized() const { return mIsInitialized.load(); }
	bool receiveMessages();
	bool sendMessages();

private:
	bool								  read(uint32_t &type, std::vector<uint8_t> &dest);

	void								  clearPendingMessages();


	std::atomic<bool>					  mIsInitialized{false};
	MessageCallback						  mCallback;

	std::shared_ptr<ISession>			  mSession;
	std::shared_ptr<SendThread>			  mSendThread;
	std::shared_ptr<ReceiveThread>		  mReceiveThread;

	std::mutex							  mIncomingListMutex;
	std::mutex							  mOutgoingListMutex;
	std::vector<netlink::InternalMessage> mIncomingMessages;
	std::vector<netlink::InternalMessage> mOutgoingMessages;
};
