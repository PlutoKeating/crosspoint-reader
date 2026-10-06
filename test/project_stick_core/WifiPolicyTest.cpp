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
