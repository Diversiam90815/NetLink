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
	RequestSent,	   // Connector: sent connect request, waiting for accept/decline
	RequestReceived,   // Acceptor:  received request, waiting for app to call respondToConnection
	Accepted,		   // Acceptor:  signaling handshake done, starting TCP listener
	AwaitingReadyFlag, // Connector: accept received, waiting for acceptor's ReadyFlag before connecting
	EstablishingTransport, // Transport role negotiation + socket setup in progress
	Connected,		   // Fully connected, messaging ready
	Disconnecting,	   // Teardown in progress
};

} // namespace netlink
