/*
  ==============================================================================
	Module:         TCPSession
	Description:    Managing the socket and session used for the multiplayer mode
  ==============================================================================
*/

#include "TCPSession.h"
#include "NetLinkLog.h"
#include "NetLinkConstants.h"


TCPSession::TCPSession(NetlinkSocket socket) : mSocket(std::move(socket)) {}


TCPSession::~TCPSession()
{
	stopReadAsync();
	mSocket.close();
}


bool TCPSession::sendMessage(netlink::InternalMessage &message)
{
	if (!isConnected())
		return false;

	auto						frame = netlink::MessageFramer::serialize(message);

	std::lock_guard<std::mutex> lock(mSendMutex);

	size_t						totalSent = 0;
	while (totalSent < frame.size())
	{
		int sent = mSocket.sendTo("", 0, frame.data() + totalSent, static_cast<int>(frame.size() - totalSent));

		if (sent <= 0)
		{
			NETLINK_LOG_ERROR("TCPSession::sendMessage failed, sent={}", sent);
			return false;
		}

		totalSent += static_cast<size_t>(sent);
	}

	return true;
}


void TCPSession::startReadAsync(MessageReceivedCallback callback)
{
	mCallback	= std::move(callback);
	mReadThread = std::make_unique<ReadThread>(this);
	mReadThread->start();
}


void TCPSession::stopReadAsync()
{
	if (mReadThread)
	{
		mReadThread->stop();
		mReadThread.reset();
	}
}


void TCPSession::pumpReceive()
{
	ReceivedPacket packet;
	if (!mSocket.receive(packet, 20))
		return;

	netlink::InternalMessage message;

	// @TODO

	if (mCallback)
		mCallback(message);
}
