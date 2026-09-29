#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "I18nKeys.h"

/**
 * StockStick i18n.
 *
 * One catalogue (Simplified Chinese) is compiled into flash. Other languages
 * are SD-card packs (`/.crosspoint/lang/<CODE>.lang`, built by
 * scripts/build_lang_pack.py) adopted at runtime. Pack entries are matched by
 * key-name hash, so packs survive firmware updates; any key a pack lacks uses
 * the built-in text. Swapping catalogues is not synchronised with readers:
 * callers hold the render lock while adopting a pack.
 */
class I18n {
 public:
  static I18n& getInstance();

  I18n(const I18n&) = delete;
  I18n& operator=(const I18n&) = delete;

  // Localised string for id; never null.
  const char* get(StrId id) const;
  const char* operator[](StrId id) const { return get(id); }

  // Take ownership of a pack image. Returns false (and keeps the current
  // catalogue) if the image is malformed.
  bool adoptPack(std::unique_ptr<uint8_t[]> image, size_t size);
  // Return to the built-in catalogue and free any pack.
  void useBuiltin();

  const char* languageCode() const;
  const char* languageName() const;
  bool usingPack() const { return pack != nullptr; }

  // Validates a pack header and copies its code/name (NUL terminated).
  static bool readPackHeader(const uint8_t* data, size_t size, char* code, size_t codeSize, char* name,
                             size_t nameSize);

  static constexpr size_t MAX_PACK_BYTES = 32 * 1024;
  static constexpr size_t HEADER_BYTES = 4 + 2 + 2 + i18n_catalogue::PACK_CODE_BYTES + i18n_catalogue::PACK_NAME_BYTES;

 private:
  I18n() = default;

  static constexpr uint16_t NO_OVERRIDE = 0xFFFF;
  std::unique_ptr<uint8_t[]> pack;
  const char* packStrings = nullptr;
  uint16_t overrides[static_cast<size_t>(StrId::_COUNT)] = {};
  char packCode[i18n_catalogue::PACK_CODE_BYTES + 1] = {};
  char packName[i18n_catalogue::PACK_NAME_BYTES + 1] = {};
};

// Convenience macros
#define tr(id) I18n::getInstance().get(StrId::id)
#define I18N I18n::getInstance()
