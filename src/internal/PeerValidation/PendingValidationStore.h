/*
  ==============================================================================
	Module:         PendingValidationStore
	Description:    Thread-safe storage for in-flight peer validations
  ==============================================================================
*/

#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "ValidationResult.h"
#include "Discovery/DiscoveryEndpoint.h"


namespace netlink
{

class PendingValidationStore
{
public:
	void							 add(const DiscoveryEndpoint &remote);
	std::optional<PendingValidation> get(const std::string &computerName) const;
	void							 remove(const std::string &computerName);
	void							 clear();

private:
	std::map<std::string, PendingValidation> mPending; // key = computerName
	mutable std::mutex						 mMutex;
};

} // namespace netlink
