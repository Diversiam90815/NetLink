/*
  ==============================================================================
	Module:         ICompatibilityCheck
	Description:    Pluggable, concurrently-evaluable peer compatibility check
  ==============================================================================
*/

#pragma once

#include <optional>
#include <string>


namespace netlink
{

enum class RemoteRequest
{
	Secret	= 1,
	Version = 2,
};


class ICompatibilityCheck
{
public:
	virtual ~ICompatibilityCheck()									   = default;

	// Stable id used for timeout keys / logging (e.g. "secret_request")
	virtual std::string					 name() const				   = 0;

	// Does this check need to ask the remote for something before it can be evaluated?
	virtual bool						 requiresRemoteRequest() const = 0;

	// If this check needs a remote round-trip over the existing signaling wire protocol,
	// this identifies which wire message type correlates to it. Checks that compute purely
	// from local state (or use a future/new wire message) may return std::nullopt.
	virtual std::optional<RemoteRequest> wireRequestType() const { return std::nullopt; }

	// Called by the orchestrator when the remote has answered our request for this check.
	virtual void						 onRemoteDataReceived(const std::string &computerName, const std::string &value) = 0;

	// True once this check has everything required to be evaluated for computerName.
	virtual bool						 isReady(const std::string &computerName) const									 = 0;

	// Evaluate compatibility. Only called once isReady() == true. Safe to run on a worker thread.
	virtual bool						 evaluate(const std::string &computerName) const								 = 0;

	// Human readable failure reason, valid after evaluate() returned false.
	virtual std::string					 failureMessage(const std::string &computerName) const							 = 0;

	// Clear any per-peer state (called after validation completes or times out).
	virtual void						 reset(const std::string &computerName)											 = 0;
};

} // namespace netlink
