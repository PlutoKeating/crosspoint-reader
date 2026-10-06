#include <gtest/gtest.h>

#include "I18n.h"
#include "I18nStrings.h"

// The firmware has one built-in catalogue (Simplified Chinese) and no runtime
// language packs (removed in 2.7.5).
TEST(I18n, BuiltinCatalogueIsChinese) {
  EXPECT_STREQ(i18n_catalogue::BUILTIN_CODE, "ZH");
  EXPECT_STREQ(tr(STR_SETTINGS_TITLE), "设置");
}

TEST(I18n, OutOfRangeIdsAreSafe) { EXPECT_STREQ(I18N.get(StrId::_COUNT), "???"); }
