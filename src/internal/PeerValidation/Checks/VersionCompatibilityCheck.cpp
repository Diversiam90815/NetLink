/*
  ==============================================================================
	Module:         VersionCompatibilityCheck
	Description:    Checking if the peer's and our app version matches
  ==============================================================================
*/


#include "VersionCompatibilityCheck.h"

netlink::VersionCompatibilityCheck::VersionCompatibilityCheck(std::string localVersion) : mLocalVersion(std::move(localVersion)) {}


void netlink::VersionCompatibilityCheck::setLocalVersion(std::string version)
{
	std::lock_guard<std::mutex> lock(mMutex);
	mLocalVersion = std::move(version);
}


void netlink::VersionCompatibilityCheck::onRemoteDataReceived(const std::string &computerName, const std::string &value)
{
	std::lock_guard<std::mutex> lock(mMutex);
	mReceivedVersions[computerName] = value;
}


bool netlink::VersionCompatibilityCheck::isReady(const std::string &computerName) const
{
	std::lock_guard<std::mutex> lock(mMutex);
	return mReceivedVersions.find(computerName) != mReceivedVersions.end();
}


bool netlink::VersionCompatibilityCheck::evaluate(const std::string &computerName) const
{
	std::lock_guard<std::mutex> lock(mMutex);

	auto						it = mReceivedVersions.find(computerName);
	if (it == mReceivedVersions.end())
		return false;

	return it->second == mLocalVersion;
}


std::string netlink::VersionCompatibilityCheck::failureMessage(const std::string &) const
{
	return "Version missmatch";
}


void netlink::VersionCompatibilityCheck::reset(const std::string &computerName)
{
	std::lock_guard<std::mutex> lock(mMutex);
	mReceivedVersions.erase(computerName);
}


std::string netlink::VersionCompatibilityCheck::remoteVersion(const std::string &computerName) const
{
	std::lock_guard<std::mutex> lock(mMutex);

	auto						it = mReceivedVersions.find(computerName);
	return it != mReceivedVersions.end() ? it->second : std::string{};
}
