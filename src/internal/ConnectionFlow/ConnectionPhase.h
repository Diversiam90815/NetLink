/*
  ==============================================================================
	Module:         ConnectionPhase
	Description:    Internal phase of the connection
  ==============================================================================
*/


#pragma once

namespace netlink
{

enum class ConnectionPhase
{
	Idle,
	RequestSent,	 // Client: sent connect request, waiting for accept/decline
	RequestReceived, // Host: received request, waiting for app to call respondToConnection
	Accepted,		 // Signaling handshake done, establishing TCP
	EstablishingTCP, // TCP role negotiation + socket setup in progress
	Connected,		 // Fully connected, messaging ready
	Disconnecting,	 // Teardown in progress
};

} // namespace netlink
