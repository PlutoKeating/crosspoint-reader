// The UI loop must not allocate: the 2.2.2 crash report shows
// ProjectStickService::now() -> parseIso8601ToShanghai building a std::string
// on a fragmented heap -> std::bad_alloc -> abort(). These tests count heap
// allocations around the hot-path helpers.
#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <new>

#include "ProjectStickCore.h"
#include "StudioProgram.h"

namespace {
std::atomic<bool> counting{false};
std::atomic<int> allocations{0};
}  // namespace

void* operator new(std::size_t size) {
  if (counting) ++allocations;
  if (void* p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {
struct CountAllocations {
  CountAllocations() {
    allocations = 0;
    counting = true;
  }
  ~CountAllocations() { counting = false; }
  int count() const { return allocations.load(); }
};
}  // namespace

using namespace project_stick;

TEST(AllocationFree, ParserDoesNotAllocate) {
  ShanghaiTime time;
  CountAllocations guard;
  EXPECT_TRUE(parseIso8601ToShanghai("2026-10-01T12:30:05.123+08:00", time));
  EXPECT_TRUE(parseIso8601ToShanghai("2026-10-01T04:30:05Z", time));
  EXPECT_TRUE(shanghaiFromUtc(2026, 10, 1, 4, 30, 5, time));
  EXPECT_EQ(guard.count(), 0);
}

TEST(AllocationFree, RejectsAnRtcThatLostPower) {
  ShanghaiTime time;
  EXPECT_FALSE(parseIso8601ToShanghai("2000-01-01T00:00:00Z", time));
  EXPECT_FALSE(time.valid);
  EXPECT_FALSE(shanghaiFromUtc(2000, 1, 1, 0, 0, 0, time));
  EXPECT_FALSE(shanghaiFromUtc(2024, 12, 31, 23, 59, 59, time));
  EXPECT_TRUE(shanghaiFromUtc(2025, 1, 1, 0, 0, 0, time));
  EXPECT_FALSE(shanghaiFromUtc(2026, 13, 1, 0, 0, 0, time));
  EXPECT_FALSE(shanghaiFromUtc(2026, 1, 1, 24, 0, 0, time));
}

TEST(AllocationFree, OffsetsAndUtcAgree) {
  ShanghaiTime a, b, c;
  ASSERT_TRUE(parseIso8601ToShanghai("2026-10-01T00:30:00+08:00", a));
  ASSERT_TRUE(parseIso8601ToShanghai("2026-09-30T16:30:00Z", b));
  ASSERT_TRUE(parseIso8601ToShanghai("2026-09-30T12:30:00-04:00", c));
  EXPECT_EQ(a.day, b.day);
  EXPECT_EQ(a.secondOfDay, b.secondOfDay);
  EXPECT_EQ(a.day, c.day);
  EXPECT_EQ(a.secondOfDay, c.secondOfDay);
  EXPECT_EQ(a.secondOfDay, 30u * 60u);
}

TEST(AllocationFree, StepIsAllocationFreeAfterWarmUp) {
  studio::Program p;
  p.defaultScene = "11111111-2222-3333-4444-555555555555";
  p.cardIds = {"aaaaaaaa-0000-0000-0000-000000000001", "aaaaaaaa-0000-0000-0000-000000000002",
               "aaaaaaaa-0000-0000-0000-000000000003"};
  p.scenes = {{p.defaultScene, {0, 1, 2}}};
  p.interval = 60;
  p.random = true;
  studio::Playback s;
  int64_t now = 1789833600;
  studio::step(p, s, now);  // warm-up: scratch buffers and scene id
  CountAllocations guard;
  for (int i = 0; i < 600; ++i) studio::step(p, s, now + i);
  EXPECT_EQ(guard.count(), 0);
}
