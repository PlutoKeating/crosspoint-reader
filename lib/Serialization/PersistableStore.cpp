#include "PersistableStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

namespace {
// Counts what ArduinoJson hands to the file so a short write is detected.
class CountingFile {
 public:
  explicit CountingFile(HalFile& file) : file(file) {}
  size_t write(uint8_t c) {
    const size_t n = file.write(c);
    written += n;
    return n;
  }
  size_t write(const uint8_t* data, size_t size) {
    const size_t n = file.write(data, size);
    written += n;
    return n;
  }
  size_t written = 0;

 private:
  HalFile& file;
};

size_t fileSizeOf(const char* path) {
  HalFile file;
  if (!Storage.openFileForRead("PERSIST", path, file)) return 0;
  const size_t size = file.size();
  file.close();
  return size;
}

void ensureParentDirectory(const char* path) {
  const String full(path);
  const int slash = full.lastIndexOf('/');
  if (slash > 0) Storage.mkdir(full.substring(0, slash).c_str(), true);
}
}  // namespace

bool PersistableStoreBase::writeDocToFile(const char* path, const JsonDocument& doc) {
  // ArduinoJson drops members silently when its allocator runs dry; such a
  // document parses fine later but is missing fields (a device credential,
  // for instance). Never write it.
  if (doc.overflowed()) {
    LOG_ERR("PERSIST", "Not writing %s: document overflowed (heap)", path);
    return false;
  }
  ensureParentDirectory(path);
  const size_t expected = measureJson(doc);
  if (expected < 2) {
    LOG_ERR("PERSIST", "Not writing %s: empty document", path);
    return false;
  }
  const String temporary = String(path) + ".tmp";
  const String backup = String(path) + ".bak";
  Storage.remove(temporary.c_str());
  {
    HalFile file;
    if (!Storage.openFileForWrite("PERSIST", temporary.c_str(), file)) {
      LOG_ERR("PERSIST", "Failed to open temporary %s", path);
      return false;
    }
    CountingFile counting(file);
    serializeJson(doc, counting);
    file.flush();
    file.close();
    if (counting.written != expected) {
      Storage.remove(temporary.c_str());
      LOG_ERR("PERSIST", "Short write of %s (%u of %u bytes)", path, (unsigned)counting.written, (unsigned)expected);
      return false;
    }
  }
  // Re-read the size: the card must hold every byte before the old file goes.
  if (fileSizeOf(temporary.c_str()) != expected) {
    Storage.remove(temporary.c_str());
    LOG_ERR("PERSIST", "Temporary %s did not verify", path);
    return false;
  }

  if (!Storage.exists(path)) {
    if (Storage.rename(temporary.c_str(), path)) return true;
    Storage.remove(temporary.c_str());
    LOG_ERR("PERSIST", "Failed to activate %s", path);
    return false;
  }

  Storage.remove(backup.c_str());
  if (!Storage.rename(path, backup.c_str())) {
    Storage.remove(temporary.c_str());
    LOG_ERR("PERSIST", "Failed to preserve %s", path);
    return false;
  }
  if (!Storage.rename(temporary.c_str(), path)) {
    Storage.rename(backup.c_str(), path);
    Storage.remove(temporary.c_str());
    LOG_ERR("PERSIST", "Failed to activate %s", path);
    return false;
  }
  // Keep the prior valid snapshot as a recovery point for interrupted writes.
  return true;
}

bool PersistableStoreBase::readDocFromFile(const char* path, JsonDocument& doc) {
  return readDocFromFileEx(path, doc) == LoadState::Loaded;
}

PersistableStoreBase::LoadState PersistableStoreBase::readDocFromFileEx(const char* path, JsonDocument& doc) {
  // Parses straight from the file: no `String` copy of the whole document,
  // whose allocation is what fails first on a fragmented heap.
  const auto readOne = [&doc](const char* candidate) {
    if (!Storage.exists(candidate)) return LoadState::Missing;
    HalFile file;
    if (!Storage.openFileForRead("PERSIST", candidate, file)) return LoadState::Unavailable;
    const size_t size = file.size();
    if (size == 0) {
      file.close();
      return LoadState::Corrupt;
    }
    doc.clear();
    const DeserializationError error = deserializeJson(doc, file);
    file.close();
    if (!error) return doc.overflowed() ? LoadState::Unavailable : LoadState::Loaded;
    if (error == DeserializationError::NoMemory) return LoadState::Unavailable;
    return LoadState::Corrupt;
  };

  const LoadState primary = readOne(path);
  if (primary == LoadState::Loaded) return primary;
  const String backup = String(path) + ".bak";
  const LoadState secondary = readOne(backup.c_str());
  if (secondary == LoadState::Loaded) {
    LOG_INF("PERSIST", "Recovered %s from backup", path);
    return secondary;
  }
  if (primary == LoadState::Missing && secondary == LoadState::Missing) return LoadState::Missing;
  // Something is on the card but unusable right now: an unreadable file wins
  // over a corrupt one, since it may read fine on the next attempt.
  const LoadState worst =
      primary == LoadState::Unavailable || secondary == LoadState::Unavailable ? LoadState::Unavailable
                                                                               : LoadState::Corrupt;
  LOG_ERR("PERSIST", "Could not load %s (%s)", path, worst == LoadState::Unavailable ? "unavailable" : "corrupt");
  return worst;
}

std::string PersistableStoreBase::extractPassword(JsonVariantConst doc, bool& needsResave) {
  bool ok = false;
  std::string pass = obfuscation::deobfuscateFromBase64(doc["password_obf"] | "", &ok);
  if (!ok) {
    // Deobfuscation failed — fall back to legacy plaintext password.
    pass = doc["password"] | "";
    if (!pass.empty()) needsResave = true;
  }
  // A successfully decoded empty string is a legitimate value; preserve as-is.
  return pass;
}
