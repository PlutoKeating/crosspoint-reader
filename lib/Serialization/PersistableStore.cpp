#include "PersistableStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

bool PersistableStoreBase::writeDocToFile(const char* path, const JsonDocument& doc) {
  Storage.mkdir("/.crosspoint");
  String json;
  serializeJson(doc, json);
  const String temporary = String(path) + ".tmp";
  const String backup = String(path) + ".bak";
  Storage.remove(temporary.c_str());
  if (!Storage.writeFile(temporary.c_str(), json)) {
    LOG_ERR("PERSIST", "Failed to write temporary %s", path);
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
  const auto readOne = [&doc](const char* candidate) {
    if (!Storage.exists(candidate)) return false;
    String json = Storage.readFile(candidate);
    if (json.isEmpty()) return false;
    doc.clear();
    return !deserializeJson(doc, json);
  };

  if (readOne(path)) return true;
  const String backup = String(path) + ".bak";
  if (readOne(backup.c_str())) {
    LOG_INF("PERSIST", "Recovered %s from backup", path);
    return true;
  }
  return false;  // Expected on first boot when neither snapshot exists.
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
