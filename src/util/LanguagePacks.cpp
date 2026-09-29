#include "LanguagePacks.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>

namespace language_packs {

namespace {
constexpr size_t MAX_PACKS = 32;

std::string packPath(const char* code) { return std::string(PACK_DIR) + "/" + code + ".lang"; }
}  // namespace

bool apply(const char* code) {
  if (!code || code[0] == '\0' || strcmp(code, i18n_catalogue::BUILTIN_CODE) == 0) {
    I18N.useBuiltin();
    return true;
  }
  const std::string path = packPath(code);
  HalFile file;
  if (!Storage.openFileForRead("I18N", path, file)) {
    LOG_ERR("I18N", "Language pack %s not found, using built-in catalogue", code);
    I18N.useBuiltin();
    return false;
  }
  const size_t size = file.fileSize();
  if (size < I18n::HEADER_BYTES || size > I18n::MAX_PACK_BYTES) {
    LOG_ERR("I18N", "Language pack %s has invalid size %u", code, static_cast<unsigned>(size));
    I18N.useBuiltin();
    return false;
  }
  // The pack stays resident (<= 32 KiB) because tr() hands out pointers into it.
  auto image = makeUniqueNoThrow<uint8_t[]>(size);
  if (!image) {
    LOG_ERR("I18N", "OOM loading language pack %s (%u bytes)", code, static_cast<unsigned>(size));
    I18N.useBuiltin();
    return false;
  }
  if (file.read(image.get(), size) != static_cast<int>(size)) {
    LOG_ERR("I18N", "Language pack %s read failed", code);
    I18N.useBuiltin();
    return false;
  }
  char headerCode[i18n_catalogue::PACK_CODE_BYTES + 1];
  if (!I18n::readPackHeader(image.get(), size, headerCode, sizeof(headerCode), nullptr, 0) ||
      strcmp(headerCode, code) != 0 || !I18N.adoptPack(std::move(image), size)) {
    LOG_ERR("I18N", "Language pack %s is invalid", code);
    I18N.useBuiltin();
    return false;
  }
  LOG_INF("I18N", "Using language pack %s (%u bytes)", code, static_cast<unsigned>(size));
  return true;
}

std::vector<PackInfo> available() {
  std::vector<PackInfo> packs;
  packs.reserve(4);
  packs.push_back({i18n_catalogue::BUILTIN_CODE, i18n_catalogue::BUILTIN_NAME});

  auto dir = Storage.open(PACK_DIR);
  if (!dir || !dir.isDirectory()) return packs;
  char filename[64];
  uint8_t header[I18n::HEADER_BYTES];
  char code[i18n_catalogue::PACK_CODE_BYTES + 1];
  char name[i18n_catalogue::PACK_NAME_BYTES + 1];
  for (auto file = dir.openNextFile(); file && packs.size() < MAX_PACKS; file = dir.openNextFile()) {
    if (file.isDirectory()) continue;
    file.getName(filename, sizeof(filename));
    if (!FsHelpers::checkFileExtension(std::string_view{filename}, ".lang")) continue;
    const size_t size = file.fileSize();
    if (file.read(header, sizeof(header)) != static_cast<int>(sizeof(header))) continue;
    // Header size check uses the real file size; the body is validated on apply().
    if (!I18n::readPackHeader(header, size < sizeof(header) ? 0 : size, code, sizeof(code), name, sizeof(name)))
      continue;
    // The loader looks packs up by code, so only "<CODE>.lang" files are selectable.
    if (strncmp(filename, code, strlen(code)) != 0 || strcmp(filename + strlen(code), ".lang") != 0) continue;
    if (strcmp(code, i18n_catalogue::BUILTIN_CODE) == 0) continue;
    packs.push_back({code, name});
  }
  return packs;
}

}  // namespace language_packs
