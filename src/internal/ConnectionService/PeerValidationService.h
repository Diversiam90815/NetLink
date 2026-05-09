/*
  ==============================================================================
	Module:         PeerValidationService
	Description:    Pre-connection peer validation with pluggable checks
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

class PeerValidationService
{
public:
	using ValidationCallback = std::function<void(const ValidationResult &)>;

	PeerValidationService()	 = default;
	~PeerValidationService() = default;

	void							setConfig(const PeerValidationConfig &config);
	void							setValidationCallback(ValidationCallback cb);

	/**
	 * @brief	Validate a peer before connecting.
	 *
	 * Checks the cache first. If uncached, runs all enabled checks synchronously
	 * and caches the result. Returns ValidationPending when async checks are active.
	 */
	ValidationResult				validatePeer(const DiscoveryEndpoint &peer);

	std::optional<ValidationResult> getCachedResult(const std::string &ip);
	void							clearCachedResult(const std::string &ip);
	void							clearAll();

private:
	bool									checkVersion(const DiscoveryEndpoint &peer, ValidationResult &result);

	PeerValidationConfig					mConfig;
	ValidationCallback						mCallback;
	std::map<std::string, ValidationResult> mValidatedPeers; // keyed by IP — one result per peer
	mutable std::mutex						mMutex;
};

} // namespace netlink
