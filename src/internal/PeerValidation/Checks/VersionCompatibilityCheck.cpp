/*
  ==============================================================================
	Module:         VersionCompatibilityCheck
	Description:    Checking if the peer's and our app version matches
  ==============================================================================
*/


#include "VersionCompatibilityCheck.h"

#include <charconv>
#include <utility>


namespace
{

// major.minor of a dotted version string. Missing or unparsable components count as 0
std::pair<int, int> protocolVersion(std::string_view version)
{
	const auto number = [](std::string_view text)
	{
		int		   value  = 0;
		const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
		return result.ec == std::errc{} ? value : 0;
	};

	const auto firstDot = version.find('.');
	const int  major	= number(version.substr(0, firstDot));

	if (firstDot == std::string_view::npos)
		return {major, 0};

	const auto rest		 = version.substr(firstDot + 1);
	const auto secondDot = rest.find('.');

	return {major, number(rest.substr(0, secondDot))};
}

} // namespace


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


bool netlink::VersionCompatibilityCheck::isCompatible(std::string_view first, std::string_view second)
{
	return protocolVersion(first) == protocolVersion(second);
}


bool netlink::VersionCompatibilityCheck::evaluate(const std::string &computerName) const
{
	std::lock_guard<std::mutex> lock(mMutex);

	auto						it = mReceivedVersions.find(computerName);
	if (it == mReceivedVersions.end())
		return false;

	return isCompatible(it->second, mLocalVersion);
}


std::string netlink::VersionCompatibilityCheck::failureMessage(const std::string &computerName) const
{
	std::lock_guard<std::mutex> lock(mMutex);

	auto						it	   = mReceivedVersions.find(computerName);
	const std::string			remote = it != mReceivedVersions.end() ? it->second : std::string{"unknown"};

	return "Version mismatch: remote " + remote + ", local " + mLocalVersion;
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
