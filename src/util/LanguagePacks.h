#pragma once

#include <I18n.h>

#include <string>
#include <vector>

// SD-card language packs for the I18n catalogue. Packs live in PACK_DIR as
// "<CODE>.lang"; the header, not the filename, identifies the language.
namespace language_packs {

constexpr const char* PACK_DIR = "/.crosspoint/lang";

struct PackInfo {
  std::string code;
  std::string name;
};

// Applies `code`: the built-in catalogue for an empty or built-in code,
// otherwise the matching SD pack. Falls back to the built-in catalogue (and
// returns false) when the pack is missing or invalid. Caller holds the render
// lock if rendering may be in progress.
bool apply(const char* code);

// Built-in language first, then valid packs found on the SD card.
std::vector<PackInfo> available();

}  // namespace language_packs
