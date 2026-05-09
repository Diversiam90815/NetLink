/*
  ==============================================================================
	Module:         ValidationResult
	Description:    Result of pre-connection peer validation
  ==============================================================================
*/

#pragma once

#include <string>

#include "Discovery/DiscoveryEndpoint.h"


namespace netlink
{

struct PeerValidationConfig
{
	bool enableVersionCheck{false}; // off until version exchange is implemented
	int	 versionRequestTimeoutMs{3000};
};


struct ValidationResult
{
	enum class Status
	{
		ReadyForConnect,
		VersionMismatch,
		ValidationPending,
		ValidationTimeout,
		PeerInvalid,
	};

	Status			  status{Status::ReadyForConnect};
	std::string		  message{};
	DiscoveryEndpoint remote{};

	std::string		  remoteVersion{}; // populated when version check is added

	bool			  isReadyToConnect() const { return status == Status::ReadyForConnect; }
};

} // namespace netlink
