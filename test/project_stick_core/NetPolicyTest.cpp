#include <ProjectStickNetPolicy.h>
#include <gtest/gtest.h>

using project_stick::classifyRequest;
using project_stick::Deadline;
using project_stick::NetFailure;
using project_stick::tlsHeapSufficient;

TEST(NetPolicy, TlsHeapNeedsBothFreeHeapAndContiguousBlock) {
  EXPECT_TRUE(tlsHeapSufficient(80 * 1024, 40 * 1024));
  EXPECT_TRUE(tlsHeapSufficient(project_stick::TLS_MIN_FREE_HEAP, project_stick::TLS_MIN_MAX_ALLOC));
  EXPECT_FALSE(tlsHeapSufficient(project_stick::TLS_MIN_FREE_HEAP - 1, 40 * 1024));
  EXPECT_FALSE(tlsHeapSufficient(120 * 1024, project_stick::TLS_MIN_MAX_ALLOC - 1));
}

TEST(NetPolicy, ClassifiesRequestOutcomes) {
  EXPECT_EQ(classifyRequest(false, 200, false), NetFailure::Clock);
  EXPECT_EQ(classifyRequest(true, -1, false), NetFailure::Network);
  EXPECT_EQ(classifyRequest(true, 0, true), NetFailure::Memory);
  EXPECT_EQ(classifyRequest(true, 429, false), NetFailure::RateLimited);
  EXPECT_EQ(classifyRequest(true, 503, false), NetFailure::Server);
  EXPECT_EQ(classifyRequest(true, 404, true), NetFailure::Server);
  EXPECT_EQ(classifyRequest(true, 200, true), NetFailure::None);
}

TEST(NetPolicy, DeadlineExpiresOnceAndSurvivesMillisWrap) {
  Deadline deadline;
  EXPECT_FALSE(deadline.expired(123));
  deadline.start(1000, 60000);
  EXPECT_TRUE(deadline.active());
  EXPECT_FALSE(deadline.expired(60999));
  EXPECT_TRUE(deadline.expired(61000));
  deadline.stop();
  EXPECT_FALSE(deadline.expired(999999));

  deadline.start(0xFFFFF000u, 60000);  // millis() wraps during the wait
  EXPECT_FALSE(deadline.expired(0x00000010u));
  EXPECT_TRUE(deadline.expired(0xFFFFF000u + 60000u));
}
