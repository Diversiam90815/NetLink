#include <gtest/gtest.h>

#include "PeerValidation/HandshakeTracker.h"

using namespace netlink;


namespace
{

DiscoveryEndpoint makeEndpoint(const std::string &name, const std::string &ip = "10.0.0.1", int port = 5000)
{
	return DiscoveryEndpoint{ip, port, name};
}

} // namespace


TEST(HandshakeTracker, BeginForDiscoveredPeer_NewEntry_ReturnsTrue)
{
	HandshakeTracker tracker;
	EXPECT_TRUE(tracker.beginForDiscoveredPeer(makeEndpoint("pc-a"))) << "beginForDiscoveredPeer() must return true when creating a brand new handshake entry";
}


TEST(HandshakeTracker, BeginForDiscoveredPeer_ExistingEntry_ReturnsFalseAndUpdatesEndpoint)
{
	HandshakeTracker tracker;
	tracker.markReceived("pc-b"); // remote reached us first, no endpoint known yet

	bool isNew = tracker.beginForDiscoveredPeer(makeEndpoint("pc-b", "10.0.0.9", 7777));

	EXPECT_FALSE(isNew) << "beginForDiscoveredPeer() must return false when an entry already exists (e.g. from markReceived)";

	auto handshake = tracker.get("pc-b");
	ASSERT_TRUE(handshake.has_value());
	EXPECT_EQ(handshake->remoteEndpoint.port, 7777) << "The remote endpoint must be filled in on the existing entry";
	EXPECT_TRUE(handshake->received) << "Prior received state must be preserved";
}


TEST(HandshakeTracker, MarkReceived_NewEntry_ReturnsTrue)
{
	HandshakeTracker tracker;
	EXPECT_TRUE(tracker.markReceived("pc-c")) << "markReceived() must return true when the remote handshake arrives before we've discovered the peer ourselves";
}


TEST(HandshakeTracker, MarkReceived_ExistingEntry_ReturnsFalse)
{
	HandshakeTracker tracker;
	tracker.beginForDiscoveredPeer(makeEndpoint("pc-d"));

	EXPECT_FALSE(tracker.markReceived("pc-d")) << "markReceived() must return false when an entry already exists";
}


TEST(HandshakeTracker, TryCompleteAndRemove_ReturnsNullopt_WhenUnknown)
{
	HandshakeTracker tracker;
	EXPECT_FALSE(tracker.tryCompleteAndRemove("unknown").has_value()) << "tryCompleteAndRemove() must return std::nullopt for a computer name with no tracked handshake";
}


TEST(HandshakeTracker, TryCompleteAndRemove_ReturnsNullopt_WhenOnlySent)
{
	HandshakeTracker tracker;
	tracker.beginForDiscoveredPeer(makeEndpoint("pc-e"));
	tracker.markSent("pc-e");

	EXPECT_FALSE(tracker.tryCompleteAndRemove("pc-e").has_value()) << "A handshake that has only been sent (not received) must not be considered complete";
	EXPECT_TRUE(tracker.get("pc-e").has_value()) << "An incomplete handshake must remain tracked, not be erased";
}


TEST(HandshakeTracker, TryCompleteAndRemove_ReturnsNullopt_WhenOnlyReceived)
{
	HandshakeTracker tracker;
	tracker.markReceived("pc-f");

	EXPECT_FALSE(tracker.tryCompleteAndRemove("pc-f").has_value()) << "A handshake that has only been received (not sent) must not be considered complete";
}


TEST(HandshakeTracker, TryCompleteAndRemove_ReturnsEndpointAndErases_WhenBothSentAndReceived)
{
	HandshakeTracker tracker;
	tracker.beginForDiscoveredPeer(makeEndpoint("pc-g", "10.0.0.55", 8888));
	tracker.markSent("pc-g");
	tracker.markReceived("pc-g");

	auto endpoint = tracker.tryCompleteAndRemove("pc-g");

	ASSERT_TRUE(endpoint.has_value()) << "A fully complete handshake (sent && received) must return the remote endpoint";
	EXPECT_EQ(endpoint->port, 8888);
	EXPECT_FALSE(tracker.get("pc-g").has_value()) << "A completed handshake must be erased from tracking";
}


TEST(HandshakeTracker, TryCompleteAndRemove_IsIdempotent_SecondCallReturnsNullopt)
{
	HandshakeTracker tracker;
	tracker.beginForDiscoveredPeer(makeEndpoint("pc-h"));
	tracker.markSent("pc-h");
	tracker.markReceived("pc-h");

	ASSERT_TRUE(tracker.tryCompleteAndRemove("pc-h").has_value());
	EXPECT_FALSE(tracker.tryCompleteAndRemove("pc-h").has_value())
		<< "Calling tryCompleteAndRemove() again after the entry was already erased must return nullopt, not resurrect it";
}


TEST(HandshakeTracker, Remove_ClearsEntry)
{
	HandshakeTracker tracker;
	tracker.beginForDiscoveredPeer(makeEndpoint("pc-i"));

	tracker.remove("pc-i");

	EXPECT_FALSE(tracker.get("pc-i").has_value());
}


TEST(HandshakeTracker, OrderIndependence_DiscoveredFirstThenReceived_CompletesCorrectly)
{
	HandshakeTracker tracker;
	tracker.beginForDiscoveredPeer(makeEndpoint("pc-j", "10.0.0.20", 4444));
	tracker.markSent("pc-j"); // simulate sendHandshake() having marked sent

	EXPECT_FALSE(tracker.tryCompleteAndRemove("pc-j").has_value());

	tracker.markReceived("pc-j");
	auto endpoint = tracker.tryCompleteAndRemove("pc-j");

	ASSERT_TRUE(endpoint.has_value());
	EXPECT_EQ(endpoint->port, 4444);
}


TEST(HandshakeTracker, OrderIndependence_ReceivedFirstThenDiscovered_CompletesCorrectly)
{
	HandshakeTracker tracker;
	tracker.markReceived("pc-k"); // remote reaches us first

	EXPECT_FALSE(tracker.tryCompleteAndRemove("pc-k").has_value());

	tracker.beginForDiscoveredPeer(makeEndpoint("pc-k", "10.0.0.30", 3333));
	tracker.markSent("pc-k");

	auto endpoint = tracker.tryCompleteAndRemove("pc-k");
	ASSERT_TRUE(endpoint.has_value());
	EXPECT_EQ(endpoint->port, 3333);
}

TEST(HandshakeTracker, IsCompleteDefaultFalse)
{
	RemoteHandshake h;
	EXPECT_FALSE(h.isComplete()) << "A default-constructed RemoteHandshake must not be complete — neither side has exchanged anything yet";
}

TEST(HandshakeTracker, IsCompleteSentOnly)
{
	RemoteHandshake h;
	h.sent = true;
	EXPECT_FALSE(h.isComplete()) << "Sending a handshake but not yet receiving one must not be considered complete";
}

TEST(HandshakeTracker, IsCompleteReceivedOnly)
{
	RemoteHandshake h;
	h.received = true;
	EXPECT_FALSE(h.isComplete()) << "Receiving a handshake but not yet sending one must not be considered complete";
}

TEST(HandshakeTracker, IsCompleteWhenBothTrue)
{
	RemoteHandshake h;
	h.sent	   = true;
	h.received = true;
	EXPECT_TRUE(h.isComplete()) << "A handshake is only complete when both the outgoing and incoming sides have been exchanged";
}
