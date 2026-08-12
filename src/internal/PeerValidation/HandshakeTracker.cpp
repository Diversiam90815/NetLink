/*
  ==============================================================================
	Module:         HandshakeTracker
	Description:    Thread-safe tracker for the two-way "handshake" exchange
  ==============================================================================
*/

#include "HandshakeTracker.h"
#include "NetLinkLog.h"


bool netlink::HandshakeTracker::beginForDiscoveredPeer(const DiscoveryEndpoint &endpoint)
{
	std::lock_guard<std::mutex> lock(mMutex);

	auto						it = mHandshakes.find(endpoint.displayName);

	if (it == mHandshakes.end())
	{
		RemoteHandshake handshake;
		handshake.remoteName			  = endpoint.displayName;
		handshake.remoteEndpoint		  = endpoint;
		handshake.sent					  = false;
		handshake.received				  = false;
		handshake.initiatedTime			  = std::chrono::steady_clock::now();

		mHandshakes[endpoint.displayName] = handshake;
		return true;
	}

	// Already tracked (e.g. remote reached us first) -> just fill in the endpoint info
	it->second.remoteEndpoint = endpoint;
	NETLINK_LOG_DEBUG("Updated existing handshake for {} with RemoteEndpoint", endpoint.displayName);
	return false;
}


bool netlink::HandshakeTracker::markReceived(const std::string &computerName)
{
	std::lock_guard<std::mutex> lock(mMutex);

	auto						it = mHandshakes.find(computerName);

	if (it != mHandshakes.end())
	{
		it->second.received = true;
		return false;
	}

	RemoteHandshake handshake;
	handshake.remoteName	  = computerName;
	handshake.received		  = true;
	handshake.sent			  = false;
	handshake.initiatedTime	  = std::chrono::steady_clock::now();

	mHandshakes[computerName] = handshake;
	return true;
}


void netlink::HandshakeTracker::markSent(const std::string &computerName)
{
	std::lock_guard<std::mutex> lock(mMutex);

	auto						it = mHandshakes.find(computerName);
	if (it != mHandshakes.end())
		it->second.sent = true;
}


std::optional<DiscoveryEndpoint> netlink::HandshakeTracker::tryCompleteAndRemove(const std::string &computerName)
{
	std::lock_guard<std::mutex> lock(mMutex);

	auto						it = mHandshakes.find(computerName);
	if (it == mHandshakes.end())
	{
		NETLINK_LOG_WARNING("Cannot start validation for {}. No pending handshake found!", computerName);
		return std::nullopt;
	}

	if (!it->second.isComplete())
	{
		NETLINK_LOG_DEBUG("Handshake incomplete for {} : sent: {}, received: {}. Waiting..", computerName, it->second.sent ? "true" : "false",
						  it->second.received ? "true" : "false");
		return std::nullopt;
	}

	DiscoveryEndpoint endpoint = it->second.remoteEndpoint;
	mHandshakes.erase(it);

	NETLINK_LOG_INFO("Remote Handshake complete with {}. Starting validation..", computerName);
	return endpoint;
}


void netlink::HandshakeTracker::remove(const std::string &computerName)
{
	std::lock_guard<std::mutex> lock(mMutex);
	mHandshakes.erase(computerName);
}


std::optional<netlink::RemoteHandshake> netlink::HandshakeTracker::get(const std::string &computerName) const
{
	std::lock_guard<std::mutex> lock(mMutex);

	auto						it = mHandshakes.find(computerName);
	if (it != mHandshakes.end())
		return it->second;

	return std::nullopt;
}
