#include <gtest/gtest.h>

#include "StudioProgram.h"
using namespace studio;
namespace {
Program sample() {
  Program p;
  p.defaultScene = "a";
  p.cardIds = {"one", "two", "alert"};
  p.scenes = {{"a", {0, 1}}, {"b", {2}}};
  p.interval = 60;
  return p;
}
}  // namespace
TEST(StudioProgram, ResumeAndBackwardClock) {
  auto p = sample();
  Playback s;
  EXPECT_EQ(step(p, s, 1000), 0);
  EXPECT_EQ(step(p, s, 1060), 1);
  auto restored = s;
  EXPECT_EQ(step(p, restored, 900), 1);
  EXPECT_EQ(step(p, restored, 1120), 0);
}
TEST(StudioProgram, OvernightBelongsToStartDay) {
  auto p = sample();
  Window w;
  w.enabled = true;
  w.start = "22:00";
  w.end = "02:00";
  w.repeat = "weekdays";
  EXPECT_TRUE(active(w, 1767979800, p));
  EXPECT_FALSE(active(w, 1767988800, p));
}
TEST(StudioProgram, AlertDoesNotDestroyManualOverride) {
  auto p = sample();
  p.alertScene = "b";
  p.alertInterrupts = true;
  Playback s;
  EXPECT_EQ(step(p, s, 1000), 0);
  EXPECT_EQ(step(p, s, 1001, 1), 1);
  EXPECT_EQ(step(p, s, 1002, 0, 1100), 2);
  EXPECT_EQ(step(p, s, 1100), 1);
}
TEST(StudioProgram, EqualPriorityHoldsScreen) {
  auto p = sample();
  Window a;
  a.id = "a";
  a.scene = "a";
  a.start = "00:00";
  a.end = "24:00";
  a.repeat = "daily";
  a.priority = 1;
  p.windows = {a, a};
  Playback s;
  EXPECT_EQ(step(p, s, 1767974400), -1);
}
TEST(StudioProgram, PortableUnlockDoesNotAdvance) {
  auto p = sample();
  p.mode = "portable";
  p.keyguard = 20;
  Playback s;
  EXPECT_EQ(step(p, s, 1800000000), 0);
  EXPECT_EQ(step(p, s, 1800000001, 1), 0);
  EXPECT_EQ(s.signal, 1);
  EXPECT_EQ(step(p, s, 1800000002, 1), 1);
  EXPECT_EQ(s.signal, 2);
}
TEST(StudioProgram, ShuffleMatchesMiniProgramFixture) {
  Scene scene{"10000000-0000-4000-8000-000000000000", {0, 1, 2, 3, 4, 5}};
  EXPECT_EQ(order(scene, 1789833600, true), (std::vector<int>{1, 5, 3, 4, 0, 2}));
}
