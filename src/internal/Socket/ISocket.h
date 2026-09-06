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
	virtual ~ISocket() = default;

	virtual NetLink::SocketBindResult bind(const std::string &address, const int port, const NetLink::BindOptions &options)		   = 0;

	virtual int						  sendTo(const std::string &address, const int port, const void *data, int size)			   = 0;
	virtual int						  recvFrom(void *buffer, int maxSize, std::string &remoteHost, int &remotePort, int timeoutMS) = 0;

	virtual bool					  waitUntilReady(bool forReading, int timeoutMS)											   = 0;

	// TCP-only lifecycle
	virtual bool					  listen(int backlog) { return false; }
	virtual bool					  accept(int timeoutMS) { return false; } // blocks until a client connects (or times out)
	virtual bool					  connect(const std::string &address, int port, int timeoutMS) { return false; }
	virtual bool					  isConnected() const { return false; }

	virtual void					  close()			   = 0;
	virtual bool					  isOpen() const	   = 0;
	virtual int						  getBoundPort() const = 0;
};
