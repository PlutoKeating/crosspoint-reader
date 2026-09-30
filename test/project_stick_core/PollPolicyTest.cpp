#include <gtest/gtest.h>

#include "ProjectStickPollPolicy.h"

using project_stick::ApiBackoff;
using project_stick::StudioPollSchedule;

TEST(PollPolicy, StudioPollSecondsClamp) {
  EXPECT_EQ(project_stick::clampStudioPollSeconds(0), 60u);
  EXPECT_EQ(project_stick::clampStudioPollSeconds(-5), 60u);
  EXPECT_EQ(project_stick::clampStudioPollSeconds(1), 15u);
  EXPECT_EQ(project_stick::clampStudioPollSeconds(120), 120u);
  EXPECT_EQ(project_stick::clampStudioPollSeconds(100000), 3600u);
}

TEST(PollPolicy, RetryAfterParsing) {
  EXPECT_EQ(project_stick::parseRetryAfterSeconds("120"), 120u);
  EXPECT_EQ(project_stick::parseRetryAfterSeconds(" 30 "), 30u);
  EXPECT_EQ(project_stick::parseRetryAfterSeconds(""), 0u);
  EXPECT_EQ(project_stick::parseRetryAfterSeconds(nullptr), 0u);
  EXPECT_EQ(project_stick::parseRetryAfterSeconds("Wed, 21 Oct 2026 07:28:00 GMT"), 0u);
  EXPECT_EQ(project_stick::parseRetryAfterSeconds("12abc"), 0u);
}

TEST(PollPolicy, BackoffStatuses) {
  EXPECT_TRUE(project_stick::isBackoffStatus(-1));
  EXPECT_TRUE(project_stick::isBackoffStatus(0));
  EXPECT_TRUE(project_stick::isBackoffStatus(429));
  EXPECT_TRUE(project_stick::isBackoffStatus(503));
  EXPECT_FALSE(project_stick::isBackoffStatus(200));
  EXPECT_FALSE(project_stick::isBackoffStatus(401));
  EXPECT_FALSE(project_stick::isBackoffStatus(409));
}

TEST(PollPolicy, ExponentialBackoffCapsAtTenMinutes) {
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

TEST(PollPolicy, RateLimitHonoursRetryAfterAndBlocksManualSync) {
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

TEST(PollPolicy, ScheduleIntervalImmediateAndBurst) {
  StudioPollSchedule schedule;
  EXPECT_TRUE(schedule.due(0));  // first poll right away
  schedule.polled(0);
  EXPECT_FALSE(schedule.due(59999));
  EXPECT_TRUE(schedule.due(60000));
  schedule.setIntervalSeconds(300);
  EXPECT_FALSE(schedule.due(60000));
  EXPECT_TRUE(schedule.due(300000));

  schedule.pollNow();
  EXPECT_TRUE(schedule.due(1000));
  schedule.polled(1000);
  EXPECT_FALSE(schedule.due(2000));

  // Manual sync: the sync itself polls, then three follow-ups 10 s apart.
  schedule.polled(10000);
  schedule.startBurst();
  uint32_t now = 10000;
  for (int i = 0; i < 3; ++i) {
    EXPECT_FALSE(schedule.due(now + 9999));
    now += 10000;
    EXPECT_TRUE(schedule.due(now));
    schedule.polled(now);
  }
  EXPECT_FALSE(schedule.due(now + 10000));  // back to the 300 s interval
  EXPECT_TRUE(schedule.due(now + 300000));
}
