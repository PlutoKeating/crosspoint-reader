#include <ProjectStickNetPolicy.h>
#include <gtest/gtest.h>

#include <cstring>

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

using project_stick::diagnoseRequest;
using project_stick::NetStage;
using project_stick::TlsClientStage;

TEST(NetPolicy, DiagnosesWhereARequestStopped) {
  EXPECT_EQ(diagnoseRequest(true, true, TlsClientStage::Ok, 0, 200).stage, NetStage::Ok);
  auto http = diagnoseRequest(true, true, TlsClientStage::Ok, 0, 503);
  EXPECT_EQ(http.stage, NetStage::Http);
  EXPECT_EQ(http.code, 503);
  EXPECT_EQ(diagnoseRequest(false, true, TlsClientStage::None, 0, -1).stage, NetStage::Wifi);
  EXPECT_EQ(diagnoseRequest(true, false, TlsClientStage::None, 0, 0).stage, NetStage::Clock);
  EXPECT_EQ(diagnoseRequest(true, true, TlsClientStage::Dns, 0, -1).stage, NetStage::Dns);
  auto tcp = diagnoseRequest(true, true, TlsClientStage::Tcp, 116, -1);
  EXPECT_EQ(tcp.stage, NetStage::Tcp);
  EXPECT_EQ(tcp.code, 116);
  auto tls = diagnoseRequest(true, true, TlsClientStage::Tls, -188, -1);
  EXPECT_EQ(tls.stage, NetStage::Tls);
  EXPECT_EQ(tls.code, -188);
  EXPECT_EQ(diagnoseRequest(true, true, TlsClientStage::TlsSetup, -125, -1).stage, NetStage::Tls);
  EXPECT_EQ(diagnoseRequest(true, true, TlsClientStage::TlsTimeout, -323, -1).stage, NetStage::TlsTimeout);
  EXPECT_EQ(diagnoseRequest(true, true, TlsClientStage::Ok, 0, -1).stage, NetStage::Read);
}

TEST(NetPolicy, FormatsTheNetLastLine) {
  project_stick::NetDiag d = diagnoseRequest(true, true, TlsClientStage::Tls, -188, -1);
  d.freeHeap = 49152;
  d.maxAlloc = 40960;
  d.rssi = -61;
  const uint8_t ip[4] = {104, 21, 32, 1};
  memcpy(&d.ipv4, ip, 4);
  char line[256];
  project_stick::formatNetDiagLine(line, sizeof line, d, "firmware_check", "2.7.9", 1791300000LL, 1234);
  EXPECT_STREQ(line,
               "fw=2.7.9 what=firmware_check stage=tls code=-188 heap=49152 max=40960 clock=1 rssi=-61 "
               "ip=104.21.32.1 utc=1791300000 uptime_ms=1234\n");
}

TEST(NetPolicy, ParsesTheMinimalVersionAnswer) {
  char v[33];
  EXPECT_TRUE(project_stick::parseVersionAnswer("{\"v\":\"2.7.9\"}", v, sizeof v));
  EXPECT_STREQ(v, "2.7.9");
  EXPECT_TRUE(project_stick::parseVersionAnswer("{ \"v\" : \"2.8.0-rc1\" , \"n\":1}", v, sizeof v));
  EXPECT_STREQ(v, "2.8.0-rc1");
  EXPECT_FALSE(project_stick::parseVersionAnswer("{\"v\":\"\"}", v, sizeof v));
  EXPECT_FALSE(project_stick::parseVersionAnswer("{\"version\":\"2.7.9\"}", v, sizeof v));
  EXPECT_FALSE(project_stick::parseVersionAnswer("{\"v\":\"2.7.9\\\"x\"}", v, sizeof v));
  EXPECT_FALSE(project_stick::parseVersionAnswer("<html>", v, sizeof v));
  char tiny[4];
  EXPECT_FALSE(project_stick::parseVersionAnswer("{\"v\":\"2.7.9\"}", tiny, sizeof tiny));
}
