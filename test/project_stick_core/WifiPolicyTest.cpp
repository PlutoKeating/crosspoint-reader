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
  n.userJob = true;
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
  n.cloudJob = true;
  EXPECT_FALSE(wifiWanted(n)) << "2.7.4: background jobs wait for the phone to leave";
  n.userJob = true;
  EXPECT_TRUE(wifiWanted(n)) << "Settings > firmware check disconnects the phone and downloads over Wi-Fi";
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

// A cloud job and a linked phone: TLS and a connected NimBLE stack do not fit
// together. 2.7.4: only a job started on the device itself makes the phone
// yield; background jobs wait for as long as it stays linked.
using project_stick::PhoneLinkAction;
using project_stick::PhoneLinkInputs;
using project_stick::phoneLinkAction;

TEST(PhoneLinkPolicy, NoPhoneMeansProceed) {
  PhoneLinkInputs in;
  EXPECT_EQ(phoneLinkAction(in), PhoneLinkAction::Proceed);
  in.userInitiated = true;
  EXPECT_EQ(phoneLinkAction(in), PhoneLinkAction::Proceed);
}

TEST(PhoneLinkPolicy, OnDeviceJobsDisconnectThePhone) {
  // Settings > Firmware update with the mini program connected.
  PhoneLinkInputs in;
  in.phoneLinked = true;
  in.userInitiated = true;
  EXPECT_EQ(phoneLinkAction(in), PhoneLinkAction::Disconnect);
}

TEST(PhoneLinkPolicy, BackgroundJobsWaitForThePhoneToLeave) {
  PhoneLinkInputs in;
  in.phoneLinked = true;
  EXPECT_EQ(phoneLinkAction(in), PhoneLinkAction::Defer);
}

// No deadlock: the on-device job asks for Wi-Fi although the phone is still
// linked when it is queued; it disconnects the phone once it runs.
TEST(PhoneLinkPolicy, OnDeviceJobBringsWifiUpWithThePhoneLinked) {
  WifiNeeds n;
  n.phoneLinked = true;
  n.heartbeatDue = true;
  n.cloudJob = true;
  EXPECT_FALSE(wifiWanted(n));
  n.userJob = true;
  EXPECT_TRUE(wifiWanted(n));
  n = {};
  n.phoneLinked = true;
  n.pageHold = true;  // the firmware or Wi-Fi page is open
  EXPECT_TRUE(wifiWanted(n));
}

// 2.7.6 field report: a device without a cloud token kept Wi-Fi up forever for
// an install outcome nothing could report, starving NimBLE of heap.
TEST(WifiPolicy, TrialNeedsWifiOnlyWithACredential) {
  using project_stick::trialNeedsWifi;
  EXPECT_FALSE(trialNeedsWifi(true, false, false));
  EXPECT_FALSE(trialNeedsWifi(false, true, false));
  EXPECT_FALSE(trialNeedsWifi(true, true, false));
  EXPECT_TRUE(trialNeedsWifi(true, false, true));
  EXPECT_TRUE(trialNeedsWifi(false, true, true));
  EXPECT_FALSE(trialNeedsWifi(false, false, true));
}

TEST(WifiPolicy, CredentialLossLeavesTheRadioOff) {
  project_stick::WifiNeeds n;  // bound, cred 0, saved network, no phone, night
  n.trialPending = project_stick::trialNeedsWifi(false, true, false);
  EXPECT_FALSE(project_stick::wifiWanted(n));
  EXPECT_TRUE(project_stick::wifiShouldPowerOff(n, 30000, 0));
}
