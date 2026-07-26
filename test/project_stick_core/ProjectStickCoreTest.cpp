#include <gtest/gtest.h>

#include "ProjectStickCore.h"

using namespace project_stick;

TEST(ProjectStickCore, ParsesProtocolTimesIntoShanghai) {
  ShanghaiTime time;
  ASSERT_TRUE(parseIso8601ToShanghai("2026-07-22T13:30:05+08:00", time));
  EXPECT_EQ(time.minuteOfDay(), 13 * 60 + 30);

  ShanghaiTime utc;
  ASSERT_TRUE(parseIso8601ToShanghai("2026-07-22T05:30:05Z", utc));
  EXPECT_EQ(utc.day, time.day);
  EXPECT_EQ(utc.secondOfDay, time.secondOfDay);
  EXPECT_EQ(formatIso8601Shanghai(time), "2026-07-22T13:30:05+08:00");
}

TEST(ProjectStickCore, AdvancesAcrossMidnight) {
  ShanghaiTime time;
  ASSERT_TRUE(parseIso8601ToShanghai("2026-07-22T23:59:30+08:00", time));
  const ShanghaiTime next = advanceTime(time, 90);
  EXPECT_EQ(next.day, time.day + 1);
  EXPECT_EQ(next.minuteOfDay(), 1);
}

TEST(ProjectStickCore, SelectsTimedThenAllDayThenEnabledFallback) {
  std::vector<ScheduleWindow> windows = {
      {.scenario = "disabled", .startMinute = 0, .endMinute = 1439, .enabled = false},
      {.scenario = "trading", .startMinute = 570, .endMinute = 690, .tradingDayOnly = true},
      {.scenario = "all_day", .allDay = true},
  };
  EXPECT_EQ(selectSchedule(windows, 600, true)->scenario, "trading");
  EXPECT_EQ(selectSchedule(windows, 600, false)->scenario, "all_day");
  EXPECT_EQ(selectSchedule(windows, 800, true)->scenario, "all_day");

  windows[2].enabled = false;
  windows.push_back({.scenario = "fallback", .startMinute = 900, .endMinute = 1000});
  EXPECT_EQ(selectSchedule(windows, 800, false)->scenario, "trading");
}

TEST(ProjectStickCore, SupportsOvernightWindows) {
  std::vector<ScheduleWindow> windows = {
      {.scenario = "overnight", .startMinute = 23 * 60, .endMinute = 60},
  };
  EXPECT_NE(selectSchedule(windows, 23 * 60 + 30, true), nullptr);
  EXPECT_NE(selectSchedule(windows, 30, true), nullptr);
  EXPECT_EQ(selectSchedule(windows, 12 * 60, true)->scenario, "overnight");  // enabled fallback
}

TEST(ProjectStickCore, WeightedSelectionAvoidsUsedCopiesUntilExhausted) {
  std::vector<ContentCopy> copies = {
      {.id = 1, .text = "one", .tone = "calm", .weight = 1},
      {.id = 2, .text = "two", .tone = "calm", .weight = 3},
  };
  EXPECT_EQ(selectCopy(copies, {1}, 0)->id, 2);
  EXPECT_EQ(selectCopy(copies, {1, 2}, 0)->id, 1);
  EXPECT_EQ(selectCopy(copies, {1, 2}, 1)->id, 2);
}

TEST(ProjectStickCore, RejectsManifestPathTraversal) {
  EXPECT_TRUE(isSafeReleasePath("content/pre_open.json"));
  EXPECT_TRUE(isSafeReleasePath("schedule.json"));
  EXPECT_FALSE(isSafeReleasePath("../settings.json"));
  EXPECT_FALSE(isSafeReleasePath("content/../../settings.json"));
  EXPECT_FALSE(isSafeReleasePath("/absolute.json"));
  EXPECT_FALSE(isSafeReleasePath("content/a b.json"));
}
