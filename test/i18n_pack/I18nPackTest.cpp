#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "I18n.h"
#include "I18nStrings.h"

namespace {

using i18n_catalogue::KEY_HASHES;

void putU16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(value & 0xFF);
  out.push_back(value >> 8);
}

void putU32(std::vector<uint8_t>& out, uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) out.push_back((value >> shift) & 0xFF);
}

// Mirrors scripts/build_lang_pack.py.
std::vector<uint8_t> makePack(const char* code, const char* name,
                              const std::vector<std::pair<uint32_t, std::string>>& entries) {
  std::vector<uint8_t> out = {'S', 'L', 'N', 'G'};
  putU16(out, i18n_catalogue::PACK_VERSION);
  putU16(out, static_cast<uint16_t>(entries.size()));
  std::string field(code);
  field.resize(i18n_catalogue::PACK_CODE_BYTES, '\0');
  out.insert(out.end(), field.begin(), field.end());
  field = name;
  field.resize(i18n_catalogue::PACK_NAME_BYTES, '\0');
  out.insert(out.end(), field.begin(), field.end());
  std::string strings;
  for (const auto& [hash, text] : entries) {
    putU32(out, hash);
    putU32(out, static_cast<uint32_t>(strings.size()));
    strings += text;
    strings.push_back('\0');
  }
  out.insert(out.end(), strings.begin(), strings.end());
  return out;
}

bool adopt(const std::vector<uint8_t>& bytes) {
  auto image = std::make_unique<uint8_t[]>(bytes.size());
  memcpy(image.get(), bytes.data(), bytes.size());
  return I18N.adoptPack(std::move(image), bytes.size());
}

class I18nPack : public ::testing::Test {
 protected:
  void TearDown() override { I18N.useBuiltin(); }
};

TEST_F(I18nPack, BuiltinCatalogueIsChinese) {
  EXPECT_STREQ(I18N.languageCode(), "ZH");
  EXPECT_STREQ(tr(STR_SETTINGS_TITLE), "设置");
  EXPECT_FALSE(I18N.usingPack());
}

TEST_F(I18nPack, OverridesKnownKeysAndFallsBackForMissingOnes) {
  const auto settings = static_cast<size_t>(StrId::STR_SETTINGS_TITLE);
  ASSERT_TRUE(adopt(makePack("EN", "English", {{KEY_HASHES[settings], "Settings"}})));
  EXPECT_STREQ(I18N.languageCode(), "EN");
  EXPECT_STREQ(I18N.languageName(), "English");
  EXPECT_STREQ(tr(STR_SETTINGS_TITLE), "Settings");
  EXPECT_STREQ(tr(STR_CANCEL), "取消");  // not in the pack
}

TEST_F(I18nPack, IgnoresKeysFromOtherFirmwareVersions) {
  const auto cancel = static_cast<size_t>(StrId::STR_CANCEL);
  ASSERT_TRUE(adopt(makePack("DE", "Deutsch", {{0x12345678u, "unbekannt"}, {KEY_HASHES[cancel], "Abbrechen"}})));
  EXPECT_STREQ(tr(STR_CANCEL), "Abbrechen");
}

TEST_F(I18nPack, DropsEntriesWhoseFormatConversionsDiffer) {
  const auto synced = static_cast<size_t>(StrId::STR_SLEEP_TIMER_VALUE_FORMAT);  // "%u"
  const auto networks = static_cast<size_t>(StrId::STR_NETWORKS_FOUND);        // "%zu"
  ASSERT_TRUE(adopt(makePack("EN", "English",
                             {{KEY_HASHES[synced], "%s minutes"}, {KEY_HASHES[networks], "%zu networks, 100%% sure"}})));
  EXPECT_STREQ(tr(STR_SLEEP_TIMER_VALUE_FORMAT), "%u 分钟");  // would read an unsigned as char*
  EXPECT_STREQ(tr(STR_NETWORKS_FOUND), "%zu networks, 100%% sure");
}

TEST_F(I18nPack, RejectsMalformedPacksAndKeepsCurrentCatalogue) {
  const auto settings = static_cast<size_t>(StrId::STR_SETTINGS_TITLE);
  auto bad = makePack("EN", "English", {{KEY_HASHES[settings], "Settings"}});
  bad.pop_back();  // last string no longer terminated
  EXPECT_FALSE(adopt(bad));
  auto wrongMagic = makePack("EN", "English", {});
  wrongMagic[0] = 'X';
  EXPECT_FALSE(adopt(wrongMagic));
  auto truncated = makePack("EN", "English", {{KEY_HASHES[settings], "Settings"}});
  truncated.resize(I18n::HEADER_BYTES + 4);
  EXPECT_FALSE(adopt(truncated));
  EXPECT_FALSE(I18N.usingPack());
  EXPECT_STREQ(tr(STR_SETTINGS_TITLE), "设置");
}

TEST_F(I18nPack, ReadsHeaderWithoutAdopting) {
  const auto pack = makePack("FR", "Français", {});
  char code[9];
  char name[33];
  ASSERT_TRUE(I18n::readPackHeader(pack.data(), pack.size(), code, sizeof(code), name, sizeof(name)));
  EXPECT_STREQ(code, "FR");
  EXPECT_STREQ(name, "Français");
  EXPECT_FALSE(I18N.usingPack());
}

}  // namespace
