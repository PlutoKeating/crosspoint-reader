#include <EpdFont.h>
#include <Utf8.h>
#include <builtinFonts/notosans_8_regular.h>
#include <builtinFonts/notosanssc_12_regular.h>
#include <builtinFonts/ubuntu_10_regular.h>
#include <gtest/gtest.h>

#include "I18nStrings.h"

namespace {

// UI and small fonts fall back to Noto Sans SC 12 (see setupDisplayAndFonts).
void expectRenderable(const EpdFont& primary, const char* label) {
  const EpdFont fallback(&notosanssc_12_regular);
  for (size_t i = 0; i < i18n_catalogue::KEY_COUNT; ++i) {
    const char* text = i18n_catalogue::BUILTIN_DATA + i18n_catalogue::BUILTIN_OFFSETS[i];
    const auto* cursor = reinterpret_cast<const uint8_t*>(text);
    uint32_t codepoint;
    while ((codepoint = utf8NextCodepoint(&cursor)) != 0) {
      if (codepoint == '\n') continue;
      EXPECT_TRUE(primary.hasCodepoint(codepoint) || fallback.hasCodepoint(codepoint))
          << label << " cannot render U+" << std::hex << codepoint << " in \"" << text << "\"";
    }
  }
}

TEST(BuiltinCatalogue, RendersWithUiFontStack) { expectRenderable(EpdFont(&ubuntu_10_regular), "UI font"); }

TEST(BuiltinCatalogue, RendersWithSmallFontStack) { expectRenderable(EpdFont(&notosans_8_regular), "small font"); }

}  // namespace
