#include <LinkTuning.h>
#include <gtest/gtest.h>

using link_tuning::Policy;

// 2.7.10: one conservative connection update per link; a phone whose link
// drops right after it is not asked again.
TEST(LinkTuning, OncePerLinkAndBlocksAfterADrop) {
  Policy policy;
  const uint8_t phone[6] = {1, 2, 3, 4, 5, 6}, other[6] = {9, 9, 9, 9, 9, 9};
  policy.linkUp();
  EXPECT_TRUE(policy.shouldRequest(phone));
  policy.requested(1000);
  EXPECT_FALSE(policy.shouldRequest(phone));  // only once per link
  // Dropped 4.2 s after the request: blamed, the phone is blocked.
  EXPECT_TRUE(policy.linkDown(phone, 5200));
  policy.linkUp();
  EXPECT_FALSE(policy.shouldRequest(phone));
  EXPECT_TRUE(policy.shouldRequest(other));
}

TEST(LinkTuning, LateDropOrNoRequestIsNotBlamed) {
  Policy policy;
  const uint8_t phone[6] = {1, 2, 3, 4, 5, 6};
  policy.linkUp();
  policy.requested(1000);
  EXPECT_FALSE(policy.linkDown(phone, 1000 + link_tuning::DROP_WINDOW_MS + 1));
  policy.linkUp();
  EXPECT_TRUE(policy.shouldRequest(phone));
  EXPECT_FALSE(policy.linkDown(phone, 50000));  // never asked on this link
  policy.linkUp();
  EXPECT_TRUE(policy.shouldRequest(phone));
}

TEST(LinkTuning, RemembersAFewPhones) {
  Policy policy;
  uint8_t peer[6] = {0};
  for (uint8_t i = 0; i < Policy::BLOCKED_PEERS + 1; ++i) {
    peer[0] = i;
    policy.linkUp();
    policy.requested(100);
    EXPECT_TRUE(policy.linkDown(peer, 200));
  }
  peer[0] = Policy::BLOCKED_PEERS;  // the newest stays blocked
  EXPECT_TRUE(policy.isBlocked(peer));
  peer[0] = 0;  // the oldest was evicted
  EXPECT_FALSE(policy.isBlocked(peer));
}
