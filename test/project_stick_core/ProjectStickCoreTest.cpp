#include <gtest/gtest.h>

#include "ProjectStickCore.h"
#include "ProjectStickSyncState.h"
#include "ProjectStickStream.h"

using namespace project_stick;

TEST(ProjectStickSyncState, FailedFirstSyncDoesNotRecordCompletedStages) {
  const SyncReport failed{
      .result = SyncResult::Failed,
      .registerAttempted = true,
      .registerSucceeded = false,
      .synchronizedAt = {},
  };

  EXPECT_FALSE(shouldRecordRegisterSuccess(failed));
  EXPECT_FALSE(shouldRecordManifestPoll(failed));
}

TEST(ProjectStickSyncState, SuccessfulSyncRecordsOnlyCompletedProtocolStages) {
  const SyncReport updated{
      .result = SyncResult::Updated,
      .registerAttempted = true,
      .registerSucceeded = true,
      .manifestAttempted = true,
      .manifestCompleted = true,
      .synchronizedAt = {},
  };

  EXPECT_TRUE(shouldRecordRegisterSuccess(updated));
  EXPECT_TRUE(shouldRecordManifestPoll(updated));
}

TEST(ProjectStickSyncState, BackgroundWorkGateRejectsRefreshWhileCloudIsQueuedOrRunning) {
  BackgroundWorkGate gate;

  EXPECT_TRUE(gate.tryQueue());
  EXPECT_TRUE(gate.pending());
  EXPECT_FALSE(gate.tryQueue());

  EXPECT_TRUE(gate.begin());
  EXPECT_TRUE(gate.running());
  EXPECT_FALSE(gate.tryQueue());

  gate.complete();
  EXPECT_FALSE(gate.pending());
  EXPECT_FALSE(gate.running());
  EXPECT_TRUE(gate.tryQueue());
}

TEST(ProjectStickSyncState, ManifestFailureDoesNotSuppressTheNextRecovery) {
  const SyncReport failed{
      .result = SyncResult::Failed,
      .registerAttempted = true,
      .registerSucceeded = true,
      .manifestAttempted = true,
      .manifestCompleted = false,
      .synchronizedAt = {},
  };

  EXPECT_TRUE(shouldRecordRegisterSuccess(failed));
  EXPECT_FALSE(shouldRecordManifestPoll(failed));
}

TEST(ProjectStickSyncState, FailedRegistrationRemainsDueAtTheNextPoll) {
  EXPECT_TRUE(registrationDue(5UL * 60UL * 1000UL, 0));
  EXPECT_FALSE(registrationDue(5UL * 60UL * 1000UL, 60UL * 1000UL));
  EXPECT_TRUE(registrationDue(4UL * 60UL * 60UL * 1000UL + 1, 1));
}

TEST(ProjectStickSyncState, NoPublishedContentRequiresNoActiveRelease) {
  EXPECT_EQ(contentSelectionFailureResult(0), SyncResult::NoContent);
  EXPECT_EQ(contentSelectionFailureResult(2), SyncResult::Failed);
}

TEST(ProjectStickSyncState, ActiveReleaseChangeInvalidatesScheduleCache) {
  EXPECT_TRUE(scheduleCacheNeedsReload(5, 6, false));
  EXPECT_TRUE(scheduleCacheNeedsReload(6, 6, true));
  EXPECT_FALSE(scheduleCacheNeedsReload(6, 6, false));
}

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

TEST(ProjectStickCore, RemovesMatchingWrappingQuotesFromDisplayCopy) {
  EXPECT_EQ(stripWrappingQuotes("\"市场永远在那里\""), "市场永远在那里");
  EXPECT_EQ(stripWrappingQuotes("“市场永远在那里”"), "市场永远在那里");
  EXPECT_EQ(stripWrappingQuotes("  “市场永远在那里”  "), "市场永远在那里");
  EXPECT_EQ(stripWrappingQuotes("市场“永远”在那里"), "市场“永远”在那里");
  EXPECT_EQ(stripWrappingQuotes("\"未闭合"), "\"未闭合");
}

TEST(ProjectStickStream, DecodesManifestAcrossSmallNetworkChunks) {
  constexpr char json[] =
      R"({"version":13,"published_at":"2026-07-22T12:00:00+08:00","poll_interval_seconds":300,"files":[)"
      R"({"path":"schedule.json","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","size":812},)"
      R"({"path":"content/pre_open.json","sha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","size":4021}]})";

  ReleaseManifestDecoder decoder;
  for (size_t offset = 0; offset < sizeof(json) - 1; offset += 7) {
    decoder.feed(json + offset, std::min<size_t>(7, sizeof(json) - 1 - offset));
  }

  ASSERT_TRUE(decoder.finish());
  EXPECT_EQ(decoder.version(), 13u);
  ASSERT_EQ(decoder.files().size(), 2u);
  EXPECT_EQ(decoder.files()[0].path, "schedule.json");
  EXPECT_EQ(decoder.files()[0].size, 812u);
  EXPECT_EQ(decoder.files()[1].path, "content/pre_open.json");
  EXPECT_EQ(decoder.files()[1].sha256, std::string(64, 'b'));
  EXPECT_EQ(decoder.pollIntervalSeconds(), 300u);
}

TEST(ProjectStickStream, StreamsManifestEntriesWithoutRetainingTheFileList) {
  constexpr char json[] =
      R"({"version":14,"files":[)"
      R"({"path":"schedule.json","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","size":8},)"
      R"({"path":"config.json","sha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","size":9}]})";
  std::vector<std::string> paths;
  ReleaseManifestDecoder decoder(
      [](void* context, const ReleaseFileEntry& file) {
        static_cast<std::vector<std::string>*>(context)->push_back(file.path);
        return true;
      },
      &paths);
  decoder.feed(json, sizeof(json) - 1);

  ASSERT_TRUE(decoder.finish());
  EXPECT_EQ(decoder.fileCount(), 2u);
  EXPECT_TRUE(decoder.files().empty());
  EXPECT_EQ(paths, (std::vector<std::string>{"schedule.json", "config.json"}));
}

TEST(ProjectStickStream, AcceptsTheDocumentedUnchangedManifest) {
  constexpr char json[] =
      R"({"unchanged":true,"version":12,"server_time":"2026-07-22T13:30:05+08:00"})";
  ReleaseManifestDecoder decoder;
  decoder.feed(json, sizeof(json) - 1);

  ASSERT_TRUE(decoder.finish());
  EXPECT_TRUE(decoder.unchanged());
  EXPECT_EQ(decoder.fileCount(), 0u);
  EXPECT_EQ(decoder.serverTime(), "2026-07-22T13:30:05+08:00");
}

TEST(ProjectStickStream, LoadsOnlyTheBoundedScheduleIntoMemory) {
  constexpr char json[] =
      R"({"trading_day_only_default":true,"windows":[)"
      R"({"scenario":"pre_open","start":"08:45","end":"09:25","enabled":true,"trading_day_only":true},)"
      R"({"scenario":"idle","start":"00:00","end":"23:59","enabled":true,"trading_day_only":false,"all_day":true}]})";

  ScheduleStreamDecoder decoder;
  for (size_t offset = 0; offset < sizeof(json) - 1; offset += 5) {
    decoder.feed(json + offset, std::min<size_t>(5, sizeof(json) - 1 - offset));
  }

  ASSERT_TRUE(decoder.finish());
  ASSERT_EQ(decoder.windows().size(), 2u);
  EXPECT_EQ(decoder.windows()[0].scenario, "pre_open");
  EXPECT_EQ(decoder.windows()[0].startMinute, 8 * 60 + 45);
  EXPECT_EQ(decoder.windows()[0].endMinute, 9 * 60 + 25);
  EXPECT_TRUE(decoder.windows()[0].tradingDayOnly);
  EXPECT_TRUE(decoder.windows()[1].allDay);
}

TEST(ProjectStickStream, SelectsOneCopyUsingTwoStreamingPasses) {
  constexpr char json[] =
      R"({"scenario":"pre_open","copies":[)"
      R"({"id":1,"text":"already shown","weight":1,"tone":"calm"},)"
      R"({"id":2,"text":"selected copy","weight":3,"tone":"focused"}]})";
  const std::vector<int64_t> usedIds = {1};

  ContentStreamDecoder measure(ContentPassMode::MEASURE, usedIds);
  for (size_t offset = 0; offset < sizeof(json) - 1; offset += 3) {
    measure.feed(json + offset, std::min<size_t>(3, sizeof(json) - 1 - offset));
  }
  ASSERT_TRUE(measure.finish());
  EXPECT_EQ(measure.totalWeight(), 4u);
  EXPECT_EQ(measure.unusedWeight(), 3u);

  ContentStreamDecoder select(ContentPassMode::SELECT_UNUSED, usedIds, 2);
  for (size_t offset = 0; offset < sizeof(json) - 1; offset += 4) {
    select.feed(json + offset, std::min<size_t>(4, sizeof(json) - 1 - offset));
  }
  ASSERT_TRUE(select.finish());
  ASSERT_NE(select.selected(), nullptr);
  EXPECT_EQ(select.selected()->id, 2);
  EXPECT_EQ(select.selected()->text, "selected copy");
  EXPECT_EQ(select.selected()->tone, "focused");
}

TEST(ProjectStickStream, FallsBackToAllCopiesAfterEveryCopyWasUsed) {
  constexpr char json[] =
      R"({"scenario":"idle","copies":[{"id":1,"text":"one","weight":1},{"id":2,"text":"two","weight":3}]})";
  const std::vector<int64_t> usedIds = {1, 2};

  ContentStreamDecoder measure(ContentPassMode::MEASURE, usedIds);
  measure.feed(json, sizeof(json) - 1);
  ASSERT_TRUE(measure.finish());
  EXPECT_EQ(measure.unusedWeight(), 0u);

  ContentStreamDecoder select(ContentPassMode::SELECT_ALL, usedIds, 1);
  select.feed(json, sizeof(json) - 1);
  ASSERT_TRUE(select.finish());
  ASSERT_NE(select.selected(), nullptr);
  EXPECT_EQ(select.selected()->id, 2);
}

TEST(ProjectStickStream, RejectsContentWithoutDisplayableText) {
  constexpr char json[] = R"({"scenario":"idle","copies":[{"id":1,"weight":1}]})";
  const std::vector<int64_t> usedIds;
  ContentStreamDecoder decoder(ContentPassMode::MEASURE, usedIds);
  decoder.feed(json, sizeof(json) - 1);
  EXPECT_FALSE(decoder.finish());
  EXPECT_FALSE(decoder.finishAllowEmpty());
}

TEST(ProjectStickStream, AcceptsEmptyOptionalContentForReleaseStorage) {
  constexpr char json[] = R"({"scenario":"manual_refresh","copies":[]})";
  const std::vector<int64_t> usedIds;
  ContentStreamDecoder decoder(ContentPassMode::MEASURE, usedIds);
  decoder.feed(json, sizeof(json) - 1);

  EXPECT_TRUE(decoder.finishAllowEmpty());
  EXPECT_FALSE(decoder.finish());
}

TEST(ProjectStickStream, RejectsMissingCopiesArrayForReleaseStorage) {
  constexpr char json[] = R"({"scenario":"manual_refresh"})";
  const std::vector<int64_t> usedIds;
  ContentStreamDecoder decoder(ContentPassMode::MEASURE, usedIds);
  decoder.feed(json, sizeof(json) - 1);

  EXPECT_FALSE(decoder.finishAllowEmpty());
}
