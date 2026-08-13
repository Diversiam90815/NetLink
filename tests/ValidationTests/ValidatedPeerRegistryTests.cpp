#include <gtest/gtest.h>
#include <thread>
#include <vector>

#include "PeerValidation/ValidatedPeerRegistry.h"

using namespace netlink;


namespace
{

ValidationResult makeResult(ValidationResult::Status status, const std::string &name)
{
	ValidationResult r;
	r.status					 = status;
	r.remoteEndpoint.displayName = name;
	r.canConnect				 = (status == ValidationResult::Status::ReadyToConnect);
	return r;
}

} // namespace


TEST(ValidatedPeerRegistry, Get_ReturnsNullopt_WhenNotPresent)
{
	ValidatedPeerRegistry registry;
	EXPECT_FALSE(registry.get("unknown").has_value());
}


TEST(ValidatedPeerRegistry, Store_ThenGet_RoundTrips)
{
	ValidatedPeerRegistry registry;
	auto				  result = makeResult(ValidationResult::Status::ReadyToConnect, "pc-a");

	registry.store("pc-a", result);

	auto retrieved = registry.get("pc-a");
	ASSERT_TRUE(retrieved.has_value());
	EXPECT_EQ(retrieved->status, ValidationResult::Status::ReadyToConnect);
	EXPECT_EQ(retrieved->remoteEndpoint.displayName, "pc-a");
}


TEST(ValidatedPeerRegistry, Store_Twice_OverwritesPreviousResult)
{
	ValidatedPeerRegistry registry;

	registry.store("pc-b", makeResult(ValidationResult::Status::SecretMissmatch, "pc-b"));
	registry.store("pc-b", makeResult(ValidationResult::Status::ReadyToConnect, "pc-b"));

	auto retrieved = registry.get("pc-b");
	ASSERT_TRUE(retrieved.has_value());
	EXPECT_EQ(retrieved->status, ValidationResult::Status::ReadyToConnect) << "A second store() call for the same peer must overwrite the previous result";
}


TEST(ValidatedPeerRegistry, Remove_ClearsEntry)
{
	ValidatedPeerRegistry registry;
	registry.store("pc-c", makeResult(ValidationResult::Status::ReadyToConnect, "pc-c"));

	registry.remove("pc-c");

	EXPECT_FALSE(registry.get("pc-c").has_value());
}


TEST(ValidatedPeerRegistry, GetAllReadyToConnect_FiltersOutNonReadyStatuses)
{
	ValidatedPeerRegistry registry;
	registry.store("pc-ready", makeResult(ValidationResult::Status::ReadyToConnect, "pc-ready"));
	registry.store("pc-secretfail", makeResult(ValidationResult::Status::SecretMissmatch, "pc-secretfail"));
	registry.store("pc-versionfail", makeResult(ValidationResult::Status::VersionMissmatch, "pc-versionfail"));
	registry.store("pc-timeout", makeResult(ValidationResult::Status::ValidationTimedout, "pc-timeout"));

	auto ready = registry.getAllReadyToConnect();

	ASSERT_EQ(ready.size(), 1u) << "Only peers with status ReadyToConnect must be returned";
	EXPECT_EQ(ready.front().remoteEndpoint.displayName, "pc-ready");
}


TEST(ValidatedPeerRegistry, GetAllReadyToConnect_EmptyWhenNoneStored)
{
	ValidatedPeerRegistry registry;
	EXPECT_TRUE(registry.getAllReadyToConnect().empty());
}


TEST(ValidatedPeerRegistry, ConcurrentStoreAndRead_IsThreadSafe)
{
	ValidatedPeerRegistry	 registry;
	constexpr int			 peerCount = 50;
	std::vector<std::thread> threads;

	for (int i = 0; i < peerCount; ++i)
	{
		threads.emplace_back([&registry, i] { registry.store("pc-" + std::to_string(i), makeResult(ValidationResult::Status::ReadyToConnect, "pc-" + std::to_string(i))); });
	}

	for (auto &t : threads)
		t.join();

	EXPECT_EQ(registry.getAllReadyToConnect().size(), static_cast<size_t>(peerCount));
}
