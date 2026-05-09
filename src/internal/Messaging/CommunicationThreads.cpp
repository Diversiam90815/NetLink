/*
  ==============================================================================
	Module:         CommunicationThreads
	Description:    Dedicated worker threads supporting RemoteCommunication.
					Each thread cooperatively waits for work signaled by RemoteCommunication (event driven).
  ==============================================================================
*/

#include "CommunicationThreads.h"
#include "RemoteCommunication.h"


SendThread::SendThread(RemoteCommunication *owner) : mOwner(owner) {}


void SendThread::run()
{
	while (isRunning())
	{
		if (!mOwner)
			return;

		mOwner->sendMessages();
		waitForEvent(200);
	}
}


ReceiveThread::ReceiveThread(RemoteCommunication *owner) : mOwner(owner) {}


void ReceiveThread::run()
{
	while (isRunning())
	{
		if (!mOwner)
			return;

		mOwner->receiveMessages();
		waitForEvent(50);
	}
}
