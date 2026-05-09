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
	ValidationPending,	   // Async peer validation in progress (@TODO)
	ValidationComplete,	   // Validation passed, ready to proceed
	RequestSent,		   // Initiator: sent ConnectRequest, awaiting Accept/Decline
	RequestReceived,	   // Responder: received request, awaiting app decision
	Accepted,			   // Signaling handshake complete
	EstablishingTransport, // Transport role determined, socket setup in progress
	AwaitingReadyFlag,	   // Transport up, waiting for both ReadyFlags
	Connected,			   // Fully connected, messaging ready
	Disconnecting,		   // Teardown in progress
};

} // namespace netlink
