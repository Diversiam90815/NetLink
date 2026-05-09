/*
  ==============================================================================
	Module:         CommunicationThreads
	Description:    Dedicated worker threads supporting RemoteCommunication.
					Each thread cooperatively waits for work signaled by RemoteCommunication (event driven).
  ==============================================================================
*/

#pragma once

#include "ThreadBase.h"

class RemoteCommunication;


class SendThread : public ThreadBase
{
public:
	explicit SendThread(RemoteCommunication *owner);

protected:
	void run() override;

private:
	RemoteCommunication *mOwner = nullptr;
};


class ReceiveThread : public ThreadBase
{
public:
	explicit ReceiveThread(RemoteCommunication *owner);

protected:
	void run() override;

private:
	RemoteCommunication *mOwner = nullptr;
};
