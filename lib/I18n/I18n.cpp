#include "I18n.h"

#include "I18nStrings.h"

using namespace i18n_catalogue;

I18n& I18n::getInstance() {
  static I18n instance;
  return instance;
}

const char* I18n::get(StrId id) const {
  const auto index = static_cast<size_t>(id);
  if (index >= KEY_COUNT) return "???";
  return BUILTIN_DATA + BUILTIN_OFFSETS[index];
}
