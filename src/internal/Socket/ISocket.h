/*
==============================================================================
	Module:         ISocket
	Description:    Low-Level socket interface
  ==============================================================================
*/

#pragma once

#include "SocketTypes.h"


class ISocket
{
public:
	virtual ~ISocket();

	virtual NetLink::SocketBindResult bind(const std::string &address, const int port, const NetLink::BindOptions &options)		   = 0;

	virtual int						  sendTo(const std::string &address, const int port, const void *data, int size)			   = 0;
	virtual int						  recvFrom(void *buffer, int maxSize, std::string &remoteHost, int &remotePort, int timeoutMS) = 0;

	virtual bool					  waitUntilReady(bool forReading, int timeoutMS)											   = 0;

	virtual void					  close()																					   = 0;

	virtual bool					  isOpen() const																			   = 0;
	virtual int						  getBoundPort() const																		   = 0;
};