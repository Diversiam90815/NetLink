/*
  ==============================================================================
	Module:         TCPSession
	Description:    Managing the socket and session used for the multiplayer mode
  ==============================================================================
*/

#pragma once

#include <vector>

#include "../Util/ThreadBase.h"
#include "Transport/TransportInterfaces.h"
#include "../Socket/NetlinkSocket.h"
#include "Messaging/MessageFramer.h"


// Concrete TCP session implementing message-based async read/write abstraction.
class TCPSession : public ISession, public std::enable_shared_from_this<TCPSession>
{
public:
	explicit TCPSession(NetlinkSocket socket);
	~TCPSession();

	bool		isConnected() const override { return mSocket.isOpen() && mSocket.isConnected(); }

	bool		sendMessage(netlink::InternalMessage &message) override;

	void		startReadAsync(MessageReceivedCallback callback) override;
	void		stopReadAsync() override;

	int			getBoundPort() const override { return mSocket.getBoundPort(); }
	std::string getRemoteAddress() const override { return mSocket.getRemoteAddress(); }
	int			getRemotePort() const override { return mSocket.getRemotePort(); }
	void		close() override
	{
		mSocket.close();
		stopReadAsync();
	}

private:
	class ReadThread : public ThreadBase
	{
	public:
		explicit ReadThread(TCPSession *owner) : mOwner(owner) {}

	protected:
		void run() override
		{
			while (isRunning())
			{
				if (mOwner)
					mOwner->pumpReceive();
				waitForEvent(20);
			}
		}

	private:
		TCPSession *mOwner = nullptr;
	};

	// blocking receive w/ short timeout, frames messages, invokes mCallback
	void						pumpReceive();

	NetlinkSocket				mSocket;
	MessageReceivedCallback		mCallback;
	std::unique_ptr<ReadThread> mReadThread;

	std::mutex					mSendMutex;
	std::vector<uint8_t>		mRecvAccumulator; // holds partial frames across calls
};
