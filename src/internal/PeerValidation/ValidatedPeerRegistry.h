/*
  ==============================================================================
	Module:         ValidatedPeerRegistry
	Description:    Thread-safe cache of completed peer validation results
  ==============================================================================
*/

#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "ValidationResult.h"


namespace netlink
{

class ValidatedPeerRegistry
{
public:
	void							store(const std::string &computerName, const ValidationResult &result);
	std::optional<ValidationResult> get(const std::string &computerName) const;
	void							remove(const std::string &computerName);
	std::vector<ValidationResult>	getAllReadyToConnect() const;

private:
	std::map<std::string, ValidationResult> mValidatedPeers; // key = computerName
	mutable std::mutex						mMutex;
};

} // namespace netlink
