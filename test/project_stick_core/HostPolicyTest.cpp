#include <ProjectStickHostPolicy.h>
#include <gtest/gtest.h>

using project_stick::shouldShowStickPage;

// A phone's request is handled on every page (ProjectStickHost); what it
// starts must also be seen. These are the page switches that follow.
TEST(HostPolicy, IncomingContentShowsTheStickPageFromOrdinaryPages) {
  EXPECT_TRUE(shouldShowStickPage(StickTakeover::Allowed, false, true));       // Settings, Home, Bluetooth...
  EXPECT_TRUE(shouldShowStickPage(StickTakeover::FirmwarePage, false, true));  // idle firmware screen
  EXPECT_FALSE(shouldShowStickPage(StickTakeover::Allowed, false, false));     // nothing to show
}

TEST(HostPolicy, TheStickPageAndLockedPagesAreNeverReplaced) {
  for (const bool updating : {false, true}) {
    for (const bool content : {false, true}) {
      EXPECT_FALSE(shouldShowStickPage(StickTakeover::StickPage, updating, content));
      // Boot, sleep, crash report, SD flashing, a firmware check in progress.
      EXPECT_FALSE(shouldShowStickPage(StickTakeover::Never, updating, content));
    }
  }
}

TEST(HostPolicy, FirmwareUpdateTakesOrdinaryPagesButNotTheFirmwareScreen) {
  EXPECT_TRUE(shouldShowStickPage(StickTakeover::Allowed, true, false));
  // The firmware screen renders the progress itself.
  EXPECT_FALSE(shouldShowStickPage(StickTakeover::FirmwarePage, true, false));
  // During an update a transfer is refused, so content never pulls the page away.
  EXPECT_FALSE(shouldShowStickPage(StickTakeover::FirmwarePage, true, true));
}
