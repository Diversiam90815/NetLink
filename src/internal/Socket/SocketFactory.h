/*
==============================================================================
	Module:         SocketFactory
	Description:    Creates platform- & UDP/TCP specific ISocket Instances
  ==============================================================================
*/

#pragma once

#include <memory>

#include "ISocket.h"
#include "SocketTypes.h"


// Returns a platform-specific ISocket for the requested transport
class SocketFactory
{
public:
	static std::unique_ptr<ISocket> create(NetLink::SocketTransport transport);
};
