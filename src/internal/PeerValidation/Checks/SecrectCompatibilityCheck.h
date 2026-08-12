/*
  ==============================================================================
	Module:         SecretCompatibilityCheck
	Description:    Checking if the peer's and our secret matches
  ==============================================================================
*/

#pragma once

#include <map>
#include <mutex>

#include "ICompatibilityCheck.h"


namespace netlink
{

class SecretCompatibilityCheck : public ICompatibilityCheck
{
public:
	explicit SecretCompatibilityCheck(std::string localSecret);

	std::string					 name() const override { return "secret_request"; }
	bool						 requiresRemoteRequest() const override { return true; }
	std::optional<RemoteRequest> wireRequestType() const override { return RemoteRequest::Secret; }

	void						 onRemoteDataReceived(const std::string &computerName, const std::string &value) override;
	bool						 isReady(const std::string &computerName) const override;
	bool						 evaluate(const std::string &computerName) const override;
	std::string					 failureMessage(const std::string &computerName) const override;
	void						 reset(const std::string &computerName) override;

	void						 setLocalSecret(std::string secret);

private:
	std::string						   mLocalSecret;
	std::map<std::string, std::string> mReceivedSecrets; // key = computerName
	mutable std::mutex				   mMutex;
};

} // namespace netlink
