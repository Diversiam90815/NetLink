/*
  ==============================================================================
	Module:         ValidationResult
	Description:    Result of pre-connection peer validation
  ==============================================================================
*/

#pragma once

#include <string>
#include <chrono>

#include "Discovery/DiscoveryEndpoint.h"


namespace netlink
{

struct PendingValidation
{
	std::string							  computerName{};
	std::string							  IPv4{};
	DiscoveryEndpoint					  remoteEndpoint{};

	std::chrono::steady_clock::time_point requestTime{};
	bool								  timedout{false};
};


struct ValidationResult
{
	enum class Status
	{
		ReadyToConnect	   = 1,
		VersionMissmatch   = 2,
		SecretMissmatch	   = 3,
		PeerInvalid		   = 4,
		AlreadyValidated   = 5,
		ResultIncomplete   = 6,
		RemoteRemoved	   = 7,
		ValidationTimedout = 8,
	};

	DiscoveryEndpoint remoteEndpoint{};
	Status			  status{};
	std::string		  message{};
	std::string		  remoteVersion{};
	bool			  canConnect{false};
	bool			  needsAction{false};

	bool			  isReadyToConnect() const { return status == Status::ReadyToConnect; }
};

} // namespace netlink
