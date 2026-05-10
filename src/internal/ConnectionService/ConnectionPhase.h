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


struct ConnectionStatusUpdate
{
	enum class Type
	{
		Initiated		   = 1,
		InvitationSent	   = 2,
		InvitationReceived = 3,
		Accepted		   = 4,
		Declined		   = 5,
		Establishing	   = 6,
		Established		   = 7,
		Failed			   = 8,
		Closing			   = 9,
		Closed			   = 10,
	};

	Type								  type{};
	ConnectionPhase						  phase{};
	DiscoveryEndpoint					  endpoint;
	std::string							  message;
	bool								  success{true};

	// Timing info
	std::chrono::steady_clock::time_point timestamp;
	std::chrono::milliseconds			  elapsed{0};

	std::string							  getTypeString() const
	{
		switch (type)
		{
		case Type::Initiated: return "Initiated";
		case Type::InvitationSent: return "Invitation Sent";
		case Type::Accepted: return "Accepted";
		case Type::Declined: return "Declined";
		case Type::Establishing: return "Establishing";
		case Type::Established: return "Established";
		case Type::Failed: return "Failed";
		case Type::Closing: return "Closing";
		case Type::Closed: return "Closed";
		default: return "Unknown type";
		}
	}
};

} // namespace netlink
