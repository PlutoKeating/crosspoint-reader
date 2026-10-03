#include <gtest/gtest.h>

#include "ProjectStickCore.h"
#include "ProjectStickKeyguard.h"
#include "ProjectStickSyncState.h"

using namespace project_stick;

TEST(ProjectStickKeyguard, LocksAfterTwentySecondsOfInactivity) {
  Keyguard keyguard;
  keyguard.begin(1000);
  EXPECT_FALSE(keyguard.update(20999));
  EXPECT_TRUE(keyguard.update(21000));
  EXPECT_EQ(keyguard.state(), Keyguard::State::AwaitLeft);
  EXPECT_FALSE(keyguard.promptVisible());
}

TEST(ProjectStickKeyguard, RevealsUnlockPromptOnlyAfterAButtonInteraction) {
  Keyguard keyguard;
  keyguard.begin(0);
  ASSERT_TRUE(keyguard.update(Keyguard::LOCK_AFTER_MS));
  EXPECT_FALSE(keyguard.promptVisible());

  EXPECT_TRUE(keyguard.update(21000, Keyguard::Input::ButtonActivity));
  EXPECT_TRUE(keyguard.promptVisible());
  EXPECT_EQ(keyguard.state(), Keyguard::State::AwaitLeft);
  EXPECT_FALSE(keyguard.update(22000));
}

TEST(ProjectStickKeyguard, ActivityRestartsTheInactivityWindow) {
  Keyguard keyguard;
  keyguard.begin(0);
  EXPECT_FALSE(keyguard.update(19000, Keyguard::Input::Activity));
  EXPECT_FALSE(keyguard.update(38999));
  EXPECT_TRUE(keyguard.update(39000));
}

TEST(ProjectStickKeyguard, UnlockRequiresLeftThenRight) {
  Keyguard keyguard;
  keyguard.begin(0);
  ASSERT_TRUE(keyguard.update(Keyguard::LOCK_AFTER_MS));
  EXPECT_TRUE(keyguard.update(21000, Keyguard::Input::RightSide));
  EXPECT_TRUE(keyguard.locked());
  EXPECT_TRUE(keyguard.promptVisible());
  EXPECT_TRUE(keyguard.update(22000, Keyguard::Input::LeftSide));
  EXPECT_EQ(keyguard.state(), Keyguard::State::AwaitRight);
  EXPECT_TRUE(keyguard.update(23000, Keyguard::Input::RightSide));
  EXPECT_FALSE(keyguard.locked());
  EXPECT_FALSE(keyguard.promptVisible());
}

TEST(ProjectStickKeyguard, RightKeyCueAnimatesOnlyAfterTheLeftStep) {
  Keyguard keyguard;
  keyguard.begin(0);
  ASSERT_TRUE(keyguard.update(Keyguard::LOCK_AFTER_MS));
  EXPECT_EQ(keyguard.cueFrame(20500), 0);
  ASSERT_TRUE(keyguard.update(21000, Keyguard::Input::LeftSide));
  EXPECT_EQ(keyguard.cueFrame(21000), 0);
  EXPECT_EQ(keyguard.cueFrame(21000 + Keyguard::CUE_FRAME_MS), 1);
  EXPECT_EQ(keyguard.cueFrame(21000 + 10 * Keyguard::CUE_FRAME_MS), Keyguard::CUE_FINAL_FRAME);
  // A repeated left release keeps the running cue instead of restarting it.
  keyguard.update(21100, Keyguard::Input::LeftSide);
  EXPECT_EQ(keyguard.cueFrame(21000 + Keyguard::CUE_FRAME_MS), 1);
  ASSERT_TRUE(keyguard.update(22000, Keyguard::Input::RightSide));
  EXPECT_EQ(keyguard.cueFrame(22500), 0);
}

TEST(ProjectStickKeyguard, UnlockPromptHidesAfterFiveIdleSeconds) {
  Keyguard keyguard;
  keyguard.begin(0);
  ASSERT_TRUE(keyguard.update(Keyguard::LOCK_AFTER_MS));
  ASSERT_TRUE(keyguard.update(21000, Keyguard::Input::ButtonActivity));
  EXPECT_TRUE(keyguard.promptVisible());
  EXPECT_FALSE(keyguard.update(21000 + Keyguard::PROMPT_TIMEOUT_MS - 1));
  EXPECT_TRUE(keyguard.promptVisible());
  EXPECT_TRUE(keyguard.update(21000 + Keyguard::PROMPT_TIMEOUT_MS));
  EXPECT_FALSE(keyguard.promptVisible());
  EXPECT_TRUE(keyguard.locked());
  // Any further key action brings the guide back with a fresh timeout.
  EXPECT_TRUE(keyguard.update(30000, Keyguard::Input::ButtonActivity));
  EXPECT_TRUE(keyguard.promptVisible());
  // A key action while the guide is up only restarts its timeout (no redraw).
  EXPECT_FALSE(keyguard.update(34000, Keyguard::Input::ButtonActivity));
  EXPECT_FALSE(keyguard.update(34000 + Keyguard::PROMPT_TIMEOUT_MS - 1));
  EXPECT_TRUE(keyguard.update(34000 + Keyguard::PROMPT_TIMEOUT_MS));
}

TEST(ProjectStickKeyguard, IdleTimeoutForgetsTheLeftStep) {
  Keyguard keyguard;
  keyguard.begin(0);
  ASSERT_TRUE(keyguard.update(Keyguard::LOCK_AFTER_MS));
  ASSERT_TRUE(keyguard.update(21000, Keyguard::Input::LeftSide));
  EXPECT_EQ(keyguard.state(), Keyguard::State::AwaitRight);
  EXPECT_TRUE(keyguard.update(21000 + Keyguard::PROMPT_TIMEOUT_MS));
  EXPECT_EQ(keyguard.state(), Keyguard::State::AwaitLeft);
  EXPECT_FALSE(keyguard.promptVisible());
  // Right alone after the timeout must not unlock.
  EXPECT_TRUE(keyguard.update(27000, Keyguard::Input::RightSide));
  EXPECT_TRUE(keyguard.locked());
}

TEST(ProjectStickKeyguard, FrontButtonRestartsAnIncompleteUnlock) {
  Keyguard keyguard;
  keyguard.begin(0);
  ASSERT_TRUE(keyguard.update(Keyguard::LOCK_AFTER_MS));
  ASSERT_TRUE(keyguard.update(21000, Keyguard::Input::LeftSide));
  EXPECT_TRUE(keyguard.update(22000, Keyguard::Input::OtherButton));
  EXPECT_EQ(keyguard.state(), Keyguard::State::AwaitLeft);
}

TEST(ProjectStickSyncState, FailedFirstSyncDoesNotRecordCompletedStages) {
  const SyncReport failed{
      .result = SyncResult::Failed,
      .registerAttempted = true,
      .registerSucceeded = false,
      .synchronizedAt = {},
  };

  EXPECT_FALSE(shouldRecordRegisterSuccess(failed));
}

TEST(ProjectStickSyncState, SuccessfulSyncRecordsTheRegistration) {
  const SyncReport synced{
      .result = SyncResult::Synced,
      .registerAttempted = true,
      .registerSucceeded = true,
      .synchronizedAt = {},
  };

  EXPECT_TRUE(shouldRecordRegisterSuccess(synced));
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

TEST(ProjectStickSyncState, FailedRegistrationRemainsDueAtTheNextPoll) {
  EXPECT_TRUE(registrationDue(5UL * 60UL * 1000UL, 0));
  EXPECT_FALSE(registrationDue(5UL * 60UL * 1000UL, 60UL * 1000UL));
  EXPECT_FALSE(registrationDue(4UL * 60UL * 60UL * 1000UL + 1, 1));
  EXPECT_TRUE(registrationDue(6UL * 60UL * 60UL * 1000UL + 1, 1));
  // A phone sync within the heartbeat interval replaces the cloud heartbeat.
  EXPECT_FALSE(phoneSyncFresh(0, 1759482000));
  EXPECT_FALSE(phoneSyncFresh(1759482000, 0));
  EXPECT_TRUE(phoneSyncFresh(1759482000, 1759482000));
  EXPECT_TRUE(phoneSyncFresh(1759482000 + 6 * 3600 - 1, 1759482000));
  EXPECT_FALSE(phoneSyncFresh(1759482000 + 6 * 3600, 1759482000));
  EXPECT_FALSE(phoneSyncFresh(1759481999, 1759482000));  // clock went backwards
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
