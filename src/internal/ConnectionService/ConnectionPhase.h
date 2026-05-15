/*
  ==============================================================================
	Module:         ConnectionState
	Description:    Internal state of the connection
  ==============================================================================
*/

#pragma once

#include "Discovery/DiscoveryEndpoint.h"


namespace netlink
{

enum class ConnectionStateInternal
{
	Idle,				   // No connection
	Initiated,			   // Started connection flow
	InvitationSent,		   // we sent invitation
	InvitationReceived,	   // we received invitation
	Accepted,			   // Invitation accepted
	Declined,			   // Invitation declined
	EstablishingTransport, // determining local session role
	AwaitingReadyFlag,	   // waiting for remote to be ready
	Connected,			   // Connection established
	Failed,				   // connection failed
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
	ConnectionStateInternal				  state{};
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
