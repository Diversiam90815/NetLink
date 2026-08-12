#include <gtest/gtest.h>
#include <chrono>

#include "PeerValidation/PendingValidationStore.h"

using namespace netlink;
using namespace std::chrono_literals;


namespace
{

DiscoveryEndpoint makeEndpoint(const std::string &name, const std::string &ip = "10.0.0.1", int port = 5000)
{
	return DiscoveryEndpoint{ip, port, name};
}

} // namespace


TEST(PendingValidationStore, Get_ReturnsNullopt_WhenNotPresent)
{
	PendingValidationStore store;
	EXPECT_FALSE(store.get("unknown").has_value()) << "get() must return std::nullopt for a computer name that was never added";
}


TEST(PendingValidationStore, Add_MakesEntryRetrievableWithCorrectFields)
{
	PendingValidationStore store;
	auto				   before = std::chrono::steady_clock::now();

	store.add(makeEndpoint("pc-a", "10.0.0.5", 6000));

	auto entry = store.get("pc-a");
	ASSERT_TRUE(entry.has_value()) << "An added entry must be retrievable via get()";
	EXPECT_EQ(entry->computerName, "pc-a");
	EXPECT_EQ(entry->IPv4, "10.0.0.5");
	EXPECT_EQ(entry->remoteEndpoint.port, 6000);
	EXPECT_GE(entry->requestTime, before) << "requestTime must be stamped at (or after) the moment add() was called";
	EXPECT_FALSE(entry->timedout) << "A freshly added entry must not be marked as timed out";
}


TEST(PendingValidationStore, Add_Twice_OverwritesPreviousEntry)
{
	PendingValidationStore store;

	store.add(makeEndpoint("pc-b", "10.0.0.1", 1111));
	store.add(makeEndpoint("pc-b", "10.0.0.2", 2222));

	auto entry = store.get("pc-b");
	ASSERT_TRUE(entry.has_value());
	EXPECT_EQ(entry->IPv4, "10.0.0.2") << "Adding the same computer name again must overwrite the prior pending entry";
}


TEST(PendingValidationStore, Remove_ClearsEntry)
{
	PendingValidationStore store;
	store.add(makeEndpoint("pc-c"));

	store.remove("pc-c");

	EXPECT_FALSE(store.get("pc-c").has_value()) << "remove() must make the entry no longer retrievable";
}


TEST(PendingValidationStore, Remove_NonExistent_DoesNotThrow)
{
	PendingValidationStore store;
	EXPECT_NO_THROW(store.remove("nonexistent")) << "Removing a name that was never added must be a safe no-op";
}


TEST(PendingValidationStore, Clear_RemovesAllEntries)
{
	PendingValidationStore store;
	store.add(makeEndpoint("pc-d"));
	store.add(makeEndpoint("pc-e"));

	store.clear();

	EXPECT_FALSE(store.get("pc-d").has_value());
	EXPECT_FALSE(store.get("pc-e").has_value());
}


TEST(PendingValidationStore, ConcurrentAddAndGet_IsThreadSafe)
{
	PendingValidationStore	 store;
	constexpr int			 peerCount = 50;
	std::vector<std::thread> threads;

	for (int i = 0; i < peerCount; ++i)
	{
		threads.emplace_back([&store, i] { store.add(makeEndpoint("pc-" + std::to_string(i), "10.0.0." + std::to_string(i), 5000 + i)); });
	}

	for (auto &t : threads)
		t.join();

	for (int i = 0; i < peerCount; ++i)
	{
		auto entry = store.get("pc-" + std::to_string(i));
		EXPECT_TRUE(entry.has_value()) << "Entry for pc-" << i << " must exist after concurrent inserts";
		if (entry)
			EXPECT_EQ(entry->remoteEndpoint.port, 5000 + i);
	}
}
