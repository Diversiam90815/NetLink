/*
==============================================================================
	Module:         SocketTypes
	Description:    Helper structs & Types for the low level socket handling
  ==============================================================================
*/

#pragma once

#include <string>

namespace NetLink
{

struct BindOptions
{
	bool enableBroadcast   = false;
	bool reuseAddress      = false;
	int  receiveBufferSize = 1024 * 1024;
	int  sendBufferSize    = 1024 * 1024;
};


struct SocketBindResult
{
	bool        success       = false;
	int         nativeError   = 0;
	int         boundPort     = 0;
	int         requestedPort = 0;
	std::string address       = "";
};


enum class SocketTransport
{
	None = 0,
	UDP = 1,
	TCP = 2
};

}