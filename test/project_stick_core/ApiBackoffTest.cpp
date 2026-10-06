#include <gtest/gtest.h>

#include "ProjectStickApiBackoff.h"

using project_stick::ApiBackoff;

TEST(ApiBackoff, RetryAfterParsing) {
  EXPECT_EQ(project_stick::parseRetryAfterSeconds("120"), 120u);
  EXPECT_EQ(project_stick::parseRetryAfterSeconds(" 30 "), 30u);
  EXPECT_EQ(project_stick::parseRetryAfterSeconds(""), 0u);
  EXPECT_EQ(project_stick::parseRetryAfterSeconds(nullptr), 0u);
  EXPECT_EQ(project_stick::parseRetryAfterSeconds("Wed, 21 Oct 2026 07:28:00 GMT"), 0u);
  EXPECT_EQ(project_stick::parseRetryAfterSeconds("12abc"), 0u);
}

TEST(ApiBackoff, BackoffStatuses) {
  EXPECT_TRUE(project_stick::isBackoffStatus(-1));
  EXPECT_TRUE(project_stick::isBackoffStatus(0));
  EXPECT_TRUE(project_stick::isBackoffStatus(429));
  EXPECT_TRUE(project_stick::isBackoffStatus(503));
  EXPECT_FALSE(project_stick::isBackoffStatus(200));
  EXPECT_FALSE(project_stick::isBackoffStatus(401));
  EXPECT_FALSE(project_stick::isBackoffStatus(409));
}

TEST(ApiBackoff, ExponentialBackoffCapsAtTenMinutes) {
  ApiBackoff backoff;
  EXPECT_FALSE(backoff.blocked(0));
  const uint32_t expected[] = {30, 60, 120, 240, 480, 600, 600};
  for (uint32_t seconds : expected) EXPECT_EQ(backoff.failure(1000, 503), seconds);
  EXPECT_TRUE(backoff.blocked(1000 + 599000));
  EXPECT_FALSE(backoff.blocked(1000 + 600000));
  backoff.record(2000, 200);
  EXPECT_FALSE(backoff.blocked(2001));
  EXPECT_EQ(backoff.failure(3000, -1), 30u);  // counter restarted after success
}

TEST(ApiBackoff, RateLimitHonoursRetryAfterAndBlocksManualSync) {
  ApiBackoff backoff;
  EXPECT_EQ(backoff.failure(0, 429, 90), 90u);
  EXPECT_TRUE(backoff.rateLimited(89000));
  EXPECT_FALSE(backoff.clearForManual(1000));
  EXPECT_TRUE(backoff.blocked(1000));
  EXPECT_FALSE(backoff.rateLimited(90000));
  EXPECT_EQ(backoff.failure(100000, 429, 86400), 600u);  // capped
  ApiBackoff errors;
  errors.failure(0, 502);
  EXPECT_TRUE(errors.clearForManual(1000));
  EXPECT_FALSE(errors.blocked(1000));
}

TEST(ApiBackoff, RateLimitWindowReportsSecondsLeftAndErrorsDoNot) {
  project_stick::ApiBackoff backoff;
  backoff.failure(1000, -1);  // a transport failure: the device's own backoff
  EXPECT_TRUE(backoff.blocked(2000));
  EXPECT_EQ(backoff.rateLimitRemainingSeconds(2000), 0u);
  backoff.failure(2000, 429, 90);
  EXPECT_EQ(backoff.rateLimitRemainingSeconds(2000), 90u);
  EXPECT_EQ(backoff.rateLimitRemainingSeconds(2000 + 89500), 1u);
  EXPECT_EQ(backoff.rateLimitRemainingSeconds(2000 + 90000), 0u);
}
