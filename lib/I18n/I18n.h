#pragma once

#include "I18nKeys.h"

/**
 * StockStick i18n.
 *
 * One catalogue (Simplified Chinese, lib/I18n/translations/chinese.yaml) is
 * compiled into flash; there are no runtime language packs (removed in
 * 2.7.5). english.yaml is kept as the checked translation reference only.
 */
class I18n {
 public:
  static I18n& getInstance();

  I18n(const I18n&) = delete;
  I18n& operator=(const I18n&) = delete;

  // Localised string for id; never null.
  const char* get(StrId id) const;
  const char* operator[](StrId id) const { return get(id); }

 private:
  I18n() = default;
};

// Convenience macros
#define tr(id) I18n::getInstance().get(StrId::id)
#define I18N I18n::getInstance()
