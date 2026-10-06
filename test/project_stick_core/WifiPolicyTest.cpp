#include <gtest/gtest.h>

#include "ProjectStickWifiPolicy.h"

using project_stick::WifiNeeds;
using project_stick::wifiShouldPowerOff;
using project_stick::wifiWanted;

TEST(WifiPolicy, IdleDeviceKeepsTheRadioDown) {
  WifiNeeds n;
  EXPECT_FALSE(wifiWanted(n));
  EXPECT_FALSE(wifiShouldPowerOff(n, 1000, 1000));
  EXPECT_TRUE(wifiShouldPowerOff(n, 1000 + project_stick::WIFI_IDLE_GRACE_MS, 1000));
}

TEST(WifiPolicy, CloudWorkBringsItUp) {
  WifiNeeds n;
  n.heartbeatDue = true;
  EXPECT_TRUE(wifiWanted(n));
  n = {};
  n.alertWindow = true;
  EXPECT_TRUE(wifiWanted(n));
  n = {};
  n.trialPending = true;
  EXPECT_TRUE(wifiWanted(n));
  n = {};
  n.cloudJob = true;
  EXPECT_TRUE(wifiWanted(n));
  n = {};
  n.otaRequested = true;
  EXPECT_TRUE(wifiWanted(n));
}

TEST(WifiPolicy, LinkedPhoneTakesTheHeapUnlessItAsksForWifi) {
  WifiNeeds n;
  n.phoneLinked = true;
  n.heartbeatDue = n.alertWindow = n.trialPending = true;
  EXPECT_FALSE(wifiWanted(n));
  EXPECT_TRUE(wifiShouldPowerOff(n, 5, 0)) << "no grace: the transfer buffers need the heap";
  n.bleJoinActive = true;
  EXPECT_TRUE(wifiWanted(n));
  n.bleJoinActive = false;
  n.joinHold = true;
  EXPECT_TRUE(wifiWanted(n));
  n.joinHold = false;
  n.otaRequested = true;
  EXPECT_TRUE(wifiWanted(n)) << "the install downloads over Wi-Fi";
}

TEST(WifiPolicy, TransferAlwaysWins) {
  WifiNeeds n;
  n.transferActive = n.phoneLinked = true;
  n.pageHold = n.bleJoinActive = n.cloudJob = n.alertWindow = true;
  EXPECT_FALSE(wifiWanted(n));
  EXPECT_TRUE(wifiShouldPowerOff(n, 0, 0));
}

TEST(WifiPolicy, PagesHoldTheRadio) {
  WifiNeeds n;
  n.pageHold = true;
  EXPECT_TRUE(wifiWanted(n));
  n.phoneLinked = true;
  EXPECT_TRUE(wifiWanted(n)) << "the firmware page needs the link while the phone follows the update";
}

// 2.7.3: a cloud job and a linked phone. TLS and a connected NimBLE stack do
// not fit together, so whoever runs the job decides who yields.
using project_stick::PhoneLinkAction;
using project_stick::PhoneLinkInputs;
using project_stick::phoneLinkAction;

TEST(PhoneLinkPolicy, NoPhoneMeansProceed) {
  PhoneLinkInputs in;
  EXPECT_EQ(phoneLinkAction(in), PhoneLinkAction::Proceed);
  in.userInitiated = true;
  EXPECT_EQ(phoneLinkAction(in), PhoneLinkAction::Proceed);
}

TEST(PhoneLinkPolicy, UserJobsDisconnectThePhoneAtOnce) {
  // Settings > Firmware update with the mini program connected, and a BLE
  // `ota` (the phone that asked is dropped so the download gets the heap),
  // even in the middle of a transfer or right after connecting.
  PhoneLinkInputs in;
  in.phoneLinked = true;
  in.userInitiated = true;
  in.linkedForMs = 1000;
  in.idleForMs = 0;
  EXPECT_EQ(phoneLinkAction(in), PhoneLinkAction::Disconnect);
  in.transferActive = true;
  EXPECT_EQ(phoneLinkAction(in), PhoneLinkAction::Disconnect);
}

TEST(PhoneLinkPolicy, BackgroundJobsWaitForAnActivePhoneThenForce) {
  PhoneLinkInputs in;
  in.phoneLinked = true;
  in.linkedForMs = 10000;
  in.idleForMs = 2000;  // the phone just read STATE
  EXPECT_EQ(phoneLinkAction(in), PhoneLinkAction::Defer);
  in.linkedForMs = project_stick::PHONE_DEFER_MAX_MS;  // a phone that never leaves
  EXPECT_EQ(phoneLinkAction(in), PhoneLinkAction::Disconnect);
}

TEST(PhoneLinkPolicy, IdlePhoneYieldsToBackgroundJobs) {
  // Trading-hours alert poll while a parked link sits idle.
  PhoneLinkInputs in;
  in.phoneLinked = true;
  in.linkedForMs = 40000;
  in.idleForMs = project_stick::PHONE_ACTIVE_WINDOW_MS;
  EXPECT_EQ(phoneLinkAction(in), PhoneLinkAction::Disconnect);
}

TEST(PhoneLinkPolicy, BackgroundJobsNeverCutATransfer) {
  PhoneLinkInputs in;
  in.phoneLinked = true;
  in.transferActive = true;
  in.linkedForMs = 10 * project_stick::PHONE_DEFER_MAX_MS;
  in.idleForMs = 10 * project_stick::PHONE_ACTIVE_WINDOW_MS;
  EXPECT_EQ(phoneLinkAction(in), PhoneLinkAction::Defer);
}

// The combined flow must not deadlock: the job that the policy lets run asks
// for Wi-Fi (cloudJob) although the phone is still linked when it is queued.
TEST(PhoneLinkPolicy, QueuedJobBringsWifiUpWithThePhoneLinked) {
  WifiNeeds n;
  n.phoneLinked = true;
  n.heartbeatDue = true;
  EXPECT_FALSE(wifiWanted(n));  // not before the policy lets the job queue
  n.cloudJob = true;            // manual check, BLE ota, or a forced heartbeat
  EXPECT_TRUE(wifiWanted(n));
  n = {};
  n.phoneLinked = true;
  n.otaRequested = true;
  EXPECT_TRUE(wifiWanted(n));
  n = {};
  n.phoneLinked = true;
  n.pageHold = true;  // the firmware page is open
  EXPECT_TRUE(wifiWanted(n));
}
