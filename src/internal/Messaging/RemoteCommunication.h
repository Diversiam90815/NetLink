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
	using MessageCallback	   = std::function<void(uint32_t type, std::vector<uint8_t> &data)>;
	using DisconnectedCallback = std::function<void(const std::string &reason)>;

	RemoteCommunication()	   = default;
	~RemoteCommunication() { deinit(); }

	bool init(std::shared_ptr<netlink::ISession> session);
	void deinit();

	void start();
	void stop();

	void write(uint32_t type, std::vector<uint8_t> data, netlink::DeliveryMode mode = netlink::DeliveryMode::ReliableOrdered);

	// Set before start()
	void setMessageCallback(MessageCallback cb) { mCallback = std::move(cb); }
	void setDisconnectedCallback(DisconnectedCallback cb) { mDisconnectedCallback = std::move(cb); }

	bool isInitialized() const { return mIsInitialized.load(); }
	bool receiveMessages();
	bool sendMessages();

private:
	void								  clearPendingMessages();


	std::atomic<bool>					  mIsInitialized{false};
	MessageCallback						  mCallback;
	DisconnectedCallback				  mDisconnectedCallback;

	std::shared_ptr<netlink::ISession>	  mSession;
	std::shared_ptr<SendThread>			  mSendThread;
	std::shared_ptr<ReceiveThread>		  mReceiveThread;

	std::mutex							  mIncomingListMutex;
	std::mutex							  mOutgoingListMutex;
	std::vector<netlink::InternalMessage> mIncomingMessages;
	std::vector<netlink::OutgoingMessage> mOutgoingMessages;
};
