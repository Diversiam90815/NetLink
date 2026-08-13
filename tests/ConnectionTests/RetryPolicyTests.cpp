#include <gtest/gtest.h>

#include "ConnectionService/ConnectionRetryPolicy.h"

using namespace netlink;


namespace ConnectionTests
{

TEST(ConnectionRetryPolicy, DefaultConstructed_AllowsRetry)
{
	ConnectionRetryPolicy policy;
	EXPECT_TRUE(policy.canRetry());
	EXPECT_EQ(policy.attempts(), 0);
}


TEST(ConnectionRetryPolicy, RecordAttempt_IncrementsCount)
{
	ConnectionRetryPolicy policy(3);

	EXPECT_TRUE(policy.recordAttempt());
	EXPECT_EQ(policy.attempts(), 1);

	EXPECT_TRUE(policy.recordAttempt());
	EXPECT_EQ(policy.attempts(), 2);
}


TEST(ConnectionRetryPolicy, RecordAttempt_ReturnsFalse_WhenLimitReached)
{
	ConnectionRetryPolicy policy(2);

	EXPECT_TRUE(policy.recordAttempt()); // attempt 1
	EXPECT_TRUE(policy.recordAttempt()); // attempt 2 (at limit)
	EXPECT_FALSE(policy.recordAttempt()) << "A third attempt must be rejected once maxRetries (2) is reached";
	EXPECT_EQ(policy.attempts(), 2) << "Attempt counter must not increment beyond the limit on a rejected attempt";
}


TEST(ConnectionRetryPolicy, CanRetry_FalseAtLimit)
{
	ConnectionRetryPolicy policy(1);

	EXPECT_TRUE(policy.canRetry());
	policy.recordAttempt();
	EXPECT_FALSE(policy.canRetry()) << "canRetry() must be false once attempts() equals maxRetries()";
}


TEST(ConnectionRetryPolicy, Reset_ClearsAttemptCount)
{
	ConnectionRetryPolicy policy(1);

	policy.recordAttempt();
	ASSERT_FALSE(policy.canRetry());

	policy.reset();

	EXPECT_TRUE(policy.canRetry()) << "reset() must restore the ability to retry";
	EXPECT_EQ(policy.attempts(), 0);
}


TEST(ConnectionRetryPolicy, SetMaxRetries_ChangesLimitWithoutResettingAttempts)
{
	ConnectionRetryPolicy policy(1);

	policy.recordAttempt(); // attempts = 1, at old limit
	ASSERT_FALSE(policy.canRetry());

	policy.setMaxRetries(3);

	EXPECT_TRUE(policy.canRetry()) << "Raising maxRetries must immediately allow further attempts";
	EXPECT_EQ(policy.attempts(), 1) << "setMaxRetries() must not reset the attempt counter";
}


TEST(ConnectionRetryPolicy, ZeroMaxRetries_NeverAllowsRetry)
{
	ConnectionRetryPolicy policy(0);

	EXPECT_FALSE(policy.canRetry());
	EXPECT_FALSE(policy.recordAttempt());
	EXPECT_EQ(policy.attempts(), 0);
}

} // namespace ConnectionTests
