/*
  ==============================================================================
	Module:         SecretCompatibilityCheck
	Description:    Checking if the peer's and our secret matches
  ==============================================================================
*/

#include "SecretCompatibilityCheck.h"


netlink::SecretCompatibilityCheck::SecretCompatibilityCheck(std::string localSecret) : mLocalSecret(std::move(localSecret)) {}


void netlink::SecretCompatibilityCheck::setLocalSecret(std::string secret)
{
	std::lock_guard<std::mutex> lock(mMutex);
	mLocalSecret = std::move(secret);
}


void netlink::SecretCompatibilityCheck::onRemoteDataReceived(const std::string &computerName, const std::string &value)
{
	std::lock_guard<std::mutex> lock(mMutex);
	mReceivedSecrets[computerName] = value;
}


bool netlink::SecretCompatibilityCheck::isReady(const std::string &computerName) const
{
	std::lock_guard<std::mutex> lock(mMutex);
	return mReceivedSecrets.find(computerName) != mReceivedSecrets.end();
}


bool netlink::SecretCompatibilityCheck::evaluate(const std::string &computerName) const
{
	std::lock_guard<std::mutex> lock(mMutex);

	auto						it = mReceivedSecrets.find(computerName);
	if (it == mReceivedSecrets.end())
		return false;

	return it->second == mLocalSecret;
}


std::string netlink::SecretCompatibilityCheck::failureMessage(const std::string &) const
{
	return "Secret missmatch";
}


void netlink::SecretCompatibilityCheck::reset(const std::string &computerName)
{
	std::lock_guard<std::mutex> lock(mMutex);
	mReceivedSecrets.erase(computerName);
}
