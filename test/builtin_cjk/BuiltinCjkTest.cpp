#include <EpdFont.h>
#include <Utf8.h>
#include <builtinFonts/notosanssc_12_regular.h>
#include <gtest/gtest.h>

namespace {

void expectTextCovered(const EpdFont& font, const char* text) {
  const auto* cursor = reinterpret_cast<const uint8_t*>(text);
  uint32_t codepoint;
  while ((codepoint = utf8NextCodepoint(&cursor)) != 0) {
    EXPECT_TRUE(font.hasCodepoint(codepoint)) << "missing U+" << std::hex << codepoint;
  }
}

}  // namespace

TEST(BuiltinCjk, CoversProjectStickContentWithoutSdFont) {
  const EpdFont font(&notosanssc_12_regular);
  expectTextCovered(font, "今天交易结束。新配置还是云，先别急着说利润。");
  expectTextCovered(font, "把过程记录下来，下次就能少猜价格的表现。");
}

TEST(BuiltinCjk, CoversMixedLatinAndChineseText) {
  const EpdFont font(&notosanssc_12_regular);
  expectTextCovered(font, "Project.Stick 已同步 v2 不好用");
}
