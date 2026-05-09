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
	bool enableVersionCheck{false};
	bool enableSecretCheck{false};
	int	 handshakeTimeoutMs{3000};
	int	 versionRequestTimeoutMs{3000};
	int	 secretRequestTimeoutMs{3000};
};


struct RemoteHandshake
{
	std::string							  remoteName{};
	DiscoveryEndpoint					  remoteEndpoint{};
	bool								  sent{false};
	bool								  received{false};
	std::chrono::steady_clock::time_point initiatedTime;

	bool								  isComplete() const { return sent && received; }
};


struct PendingValidation
{
	std::string							  computerName{};
	std::string							  IPv4{};
	DiscoveryEndpoint					  remoteEndpoint{};

	bool								  versionReceived{false};
	std::string							  version{};

	bool								  secretReceived{false};
	std::string							  secret{};

	std::chrono::steady_clock::time_point requestTime{};
	bool								  timedout{false};

	bool								  isComplete() const { return versionReceived && secretReceived; }
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

	Status			  status{};
	std::string		  message{};
	std::string		  remoteVersion{};
	DiscoveryEndpoint remoteEndpoint{};
	bool			  canConnect{false};
	bool			  needsAction{false};

	bool			  isReadyToConnect() const { return status == Status::ReadyToConnect; }
};

} // namespace netlink
