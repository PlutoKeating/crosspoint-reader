#include "I18n.h"

#include <cstring>

#include "I18nStrings.h"

using namespace i18n_catalogue;

namespace {
uint16_t readU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t readU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

void copyField(char* dest, size_t destSize, const uint8_t* src, size_t srcSize) {
  const size_t limit = destSize - 1 < srcSize ? destSize - 1 : srcSize;
  size_t length = 0;
  while (length < limit && src[length] != 0) ++length;
  memcpy(dest, src, length);
  dest[length] = '\0';
}

// True when `a` and `b` contain the same sequence of printf conversions
// (length modifier + conversion letter; flags, width and precision ignored).
// Callers pass these strings to snprintf with arguments fixed by the firmware,
// so a pack may reword text but never change what the arguments are read as.
bool sameConversions(const char* a, const char* b) {
  auto next = [](const char*& p, char* spec, size_t specSize) {
    while (*p) {
      if (*p++ != '%') continue;
      if (*p == '%') {
        ++p;
        continue;
      }
      while (*p && strchr("-+ #0123456789.*", *p)) ++p;
      size_t length = 0;
      while (*p && strchr("hlLqjzt", *p) && length + 2 < specSize) spec[length++] = *p++;
      if (!*p) return false;
      spec[length++] = *p++;
      spec[length] = '\0';
      return true;
    }
    return false;
  };
  char specA[8];
  char specB[8];
  for (;;) {
    const bool hasA = next(a, specA, sizeof(specA));
    const bool hasB = next(b, specB, sizeof(specB));
    if (hasA != hasB) return false;
    if (!hasA) return true;
    if (strcmp(specA, specB) != 0) return false;
  }
}

int findKey(uint32_t hash) {
  for (size_t i = 0; i < KEY_COUNT; ++i) {
    if (KEY_HASHES[i] == hash) return static_cast<int>(i);
  }
  return -1;
}
}  // namespace

I18n& I18n::getInstance() {
  static I18n instance;
  return instance;
}

const char* I18n::get(StrId id) const {
  const auto index = static_cast<size_t>(id);
  if (index >= KEY_COUNT) return "???";
  if (pack && overrides[index] != NO_OVERRIDE) return packStrings + overrides[index];
  return BUILTIN_DATA + BUILTIN_OFFSETS[index];
}

bool I18n::readPackHeader(const uint8_t* data, size_t size, char* code, size_t codeSize, char* name, size_t nameSize) {
  if (!data || size < HEADER_BYTES || size > MAX_PACK_BYTES) return false;
  if (memcmp(data, PACK_MAGIC, sizeof(PACK_MAGIC)) != 0 || readU16(data + 4) != PACK_VERSION) return false;
  if (code && codeSize) copyField(code, codeSize, data + 8, PACK_CODE_BYTES);
  if (name && nameSize) copyField(name, nameSize, data + 8 + PACK_CODE_BYTES, PACK_NAME_BYTES);
  return !code || code[0] != '\0';
}

bool I18n::adoptPack(std::unique_ptr<uint8_t[]> image, size_t size) {
  char code[sizeof(packCode)];
  char name[sizeof(packName)];
  if (!image || !readPackHeader(image.get(), size, code, sizeof(code), name, sizeof(name))) return false;
  const size_t count = readU16(image.get() + 6);
  const size_t tableBytes = count * 8;
  if (HEADER_BYTES + tableBytes > size) return false;
  const uint8_t* table = image.get() + HEADER_BYTES;
  const size_t stringsOffset = HEADER_BYTES + tableBytes;
  const size_t stringsSize = size - stringsOffset;
  if (stringsSize == 0 || image[size - 1] != 0) return false;  // every string must be terminated in-bounds

  uint16_t mapped[static_cast<size_t>(StrId::_COUNT)];
  for (auto& entry : mapped) entry = NO_OVERRIDE;
  for (size_t i = 0; i < count; ++i) {
    const uint32_t offset = readU32(table + i * 8 + 4);
    if (offset >= stringsSize || offset >= NO_OVERRIDE) return false;
    const int key = findKey(readU32(table + i * 8));
    if (key < 0) continue;  // unknown keys belong to other firmware versions
    const char* text = reinterpret_cast<const char*>(image.get() + stringsOffset + offset);
    if (!sameConversions(text, BUILTIN_DATA + BUILTIN_OFFSETS[key])) continue;  // keep the built-in text
    mapped[key] = static_cast<uint16_t>(offset);
  }

  pack = std::move(image);
  packStrings = reinterpret_cast<const char*>(pack.get() + stringsOffset);
  memcpy(overrides, mapped, sizeof(overrides));
  memcpy(packCode, code, sizeof(packCode));
  memcpy(packName, name, sizeof(packName));
  return true;
}

void I18n::useBuiltin() {
  pack.reset();
  packStrings = nullptr;
  packCode[0] = '\0';
  packName[0] = '\0';
}

const char* I18n::languageCode() const { return pack ? packCode : BUILTIN_CODE; }

const char* I18n::languageName() const { return pack ? packName : BUILTIN_NAME; }
