/*
  ==============================================================================
	Module:         VersionCompatibilityCheck
	Description:    Checking if the peer's and our app version matches
  ==============================================================================
*/

#pragma once

#include <map>
#include <mutex>

#include "ICompatibilityCheck.h"


namespace netlink
{

class VersionCompatibilityCheck : public ICompatibilityCheck
{
public:
	explicit VersionCompatibilityCheck(std::string localVersion);

	std::string					 name() const override { return "version_request"; }
	bool						 requiresRemoteRequest() const override { return true; }
	std::optional<RemoteRequest> wireRequestType() const override { return RemoteRequest::Version; }

	void						 onRemoteDataReceived(const std::string &computerName, const std::string &value) override;
	bool						 isReady(const std::string &computerName) const override;
	bool						 evaluate(const std::string &computerName) const override;
	std::string					 failureMessage(const std::string &computerName) const override;
	void						 reset(const std::string &computerName) override;

	void						 setLocalVersion(std::string version);
	std::string					 remoteVersion(const std::string &computerName) const;

private:
	std::string						   mLocalVersion;
	std::map<std::string, std::string> mReceivedVersions; // key = computerName
	mutable std::mutex				   mMutex;
};

} // namespace netlink
