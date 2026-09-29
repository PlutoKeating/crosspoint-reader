#include <StickFirmware.h>
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

using namespace stick_fw;

namespace {

constexpr uint16_t CHIP_C3 = 5;

std::vector<uint8_t> makeImage(const char* product, const char* version, uint32_t build,
                               uint32_t boards = BOARD_X3 | BOARD_X4, uint16_t chip = CHIP_C3) {
  std::vector<uint8_t> image(IDENTIFY_BYTES, 0);
  image[0] = 0xE9;
  image[1] = 6;
  memcpy(&image[12], &chip, sizeof(chip));
  const uint32_t appMagic = ESP_APP_DESC_MAGIC;
  memcpy(&image[APP_DESC_OFFSET], &appMagic, sizeof(appMagic));
  Descriptor desc{};
  desc.magic = DESCRIPTOR_MAGIC;
  desc.descriptorVersion = DESCRIPTOR_VERSION;
  desc.descriptorSize = sizeof(Descriptor);
  desc.build = build;
  desc.boards = boards;
  strncpy(desc.product, product, sizeof(desc.product) - 1);
  strncpy(desc.version, version, sizeof(desc.version) - 1);
  strncpy(desc.commit, "abc1234", sizeof(desc.commit) - 1);
  memcpy(&image[DESCRIPTOR_OFFSET], &desc, sizeof(desc));
  return image;
}

InstallPolicy policy(const char* expected = nullptr) {
  InstallPolicy p;
  p.chipId = CHIP_C3;
  p.board = BOARD_X3;
  p.minimumBuild = 20000;
  p.expectedVersion = expected;
  return p;
}

}  // namespace

TEST(StickFirmware, IdentifiesStockStickImage) {
  const auto image = makeImage("stockstick", "2.0.0", 20000);
  ImageInfo info;
  ASSERT_EQ(identifyImage(image.data(), image.size(), info), IdentifyResult::Ok);
  EXPECT_EQ(info.build, 20000u);
  EXPECT_STREQ(info.version, "2.0.0");
  EXPECT_STREQ(info.commit, "abc1234");
  EXPECT_EQ(checkInstall(IdentifyResult::Ok, info, policy("2.0.0")), InstallVerdict::Ok);
}

TEST(StickFirmware, RejectsUpstreamAndForeignImages) {
  auto upstream = makeImage("stockstick", "1.5.0", 1);
  memset(&upstream[DESCRIPTOR_OFFSET], 0, sizeof(Descriptor));  // upstream has no custom descriptor
  ImageInfo info;
  EXPECT_EQ(identifyImage(upstream.data(), upstream.size(), info), IdentifyResult::NotStockStick);
  EXPECT_EQ(checkInstall(IdentifyResult::NotStockStick, info, policy()), InstallVerdict::NotStockStick);

  const auto other = makeImage("crosspoint", "2.0.0", 20000);
  EXPECT_EQ(identifyImage(other.data(), other.size(), info), IdentifyResult::NotStockStick);

  auto notEsp = makeImage("stockstick", "2.0.0", 20000);
  notEsp[0] = 0;
  EXPECT_EQ(identifyImage(notEsp.data(), notEsp.size(), info), IdentifyResult::NotAnEspImage);
  EXPECT_EQ(identifyImage(notEsp.data(), 10, info), IdentifyResult::TooShort);
}

TEST(StickFirmware, RejectsUnterminatedDescriptorStrings) {
  auto image = makeImage("stockstick", "2.0.0", 20000);
  Descriptor desc;
  memcpy(&desc, &image[DESCRIPTOR_OFFSET], sizeof(desc));
  memset(desc.version, 'x', sizeof(desc.version));
  memcpy(&image[DESCRIPTOR_OFFSET], &desc, sizeof(desc));
  ImageInfo info;
  EXPECT_EQ(identifyImage(image.data(), image.size(), info), IdentifyResult::BadDescriptor);
}

TEST(StickFirmware, InstallPolicyGuardsChipBoardFloorAndCatalogue) {
  ImageInfo info;
  auto image = makeImage("stockstick", "2.0.0", 20000, BOARD_X4, 1);
  ASSERT_EQ(identifyImage(image.data(), image.size(), info), IdentifyResult::Ok);
  EXPECT_EQ(checkInstall(IdentifyResult::Ok, info, policy()), InstallVerdict::WrongChip);

  image = makeImage("stockstick", "2.0.0", 20000, BOARD_X4);
  ASSERT_EQ(identifyImage(image.data(), image.size(), info), IdentifyResult::Ok);
  EXPECT_EQ(checkInstall(IdentifyResult::Ok, info, policy()), InstallVerdict::UnsupportedBoard);

  image = makeImage("stockstick", "1.9.0", 19900);
  ASSERT_EQ(identifyImage(image.data(), image.size(), info), IdentifyResult::Ok);
  EXPECT_EQ(checkInstall(IdentifyResult::Ok, info, policy()), InstallVerdict::BelowMinimumBuild);

  image = makeImage("stockstick", "2.0.1", 20001);
  ASSERT_EQ(identifyImage(image.data(), image.size(), info), IdentifyResult::Ok);
  EXPECT_EQ(checkInstall(IdentifyResult::Ok, info, policy("2.0.2")), InstallVerdict::VersionMismatch);
  EXPECT_EQ(checkInstall(IdentifyResult::Ok, info, policy(nullptr)), InstallVerdict::Ok);
}

TEST(StickFirmware, TrialBootCountsOnlyAbnormalResets) {
  TrialRecord record;
  EXPECT_EQ(decideOnBoot(record, "app1", false), BootAction::None);

  record.armed = true;
  strcpy(record.previousSlot, "app0");
  strcpy(record.targetSlot, "app1");
  EXPECT_EQ(decideOnBoot(record, "app1", false), BootAction::Continue);  // first boot after install
  EXPECT_EQ(decideOnBoot(record, "app1", false), BootAction::Continue);  // deep-sleep wake
  EXPECT_EQ(record.attempts, 0);
  EXPECT_EQ(decideOnBoot(record, "app1", true), BootAction::Continue);
  EXPECT_EQ(decideOnBoot(record, "app1", true), BootAction::Continue);
  EXPECT_EQ(decideOnBoot(record, "app1", true), BootAction::RollbackNow);
}

TEST(StickFirmware, TrialDetectsBootloaderRollback) {
  TrialRecord record;
  record.armed = true;
  strcpy(record.previousSlot, "app0");
  strcpy(record.targetSlot, "app1");
  EXPECT_EQ(decideOnBoot(record, "app0", true), BootAction::RolledBack);
}

TEST(StickFirmware, HealthPolicy) {
  HealthInputs in;
  EXPECT_EQ(evaluateHealth(in), HealthDecision::Wait);

  in.apiResponded = true;
  EXPECT_EQ(evaluateHealth(in), HealthDecision::Confirm);

  in = {};
  in.everOnline = true;
  in.transportFailures = ONLINE_FAILURE_LIMIT;
  in.failureSpanMs = ONLINE_FAILURE_WINDOW_MS - 1;
  EXPECT_EQ(evaluateHealth(in), HealthDecision::Wait);
  in.failureSpanMs = ONLINE_FAILURE_WINDOW_MS;
  EXPECT_EQ(evaluateHealth(in), HealthDecision::Rollback);

  in = {};
  in.uptimeMs = OFFLINE_CONFIRM_MS;
  EXPECT_EQ(evaluateHealth(in), HealthDecision::Confirm);  // never online: nothing else can judge the build

  in.everOnline = true;
  EXPECT_EQ(evaluateHealth(in), HealthDecision::Wait);
}
