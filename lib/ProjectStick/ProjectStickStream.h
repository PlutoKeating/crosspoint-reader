#pragma once

#include "ProjectStickCore.h"

#include <StreamingJsonParser.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace project_stick {

struct ReleaseFileEntry {
  std::string path;
  std::string sha256;
  size_t size = 0;
};

class ReleaseManifestDecoder {
 public:
  static constexpr size_t MAX_FILES = 64;
  using FileCallback = bool (*)(void* context, const ReleaseFileEntry& file);

  explicit ReleaseManifestDecoder(FileCallback fileCallback = nullptr, void* callbackContext = nullptr);

  void feed(const char* data, size_t length);
  bool finish() const;

  uint32_t version() const { return releaseVersion; }
  bool unchanged() const { return releaseUnchanged; }
  size_t fileCount() const { return decodedFileCount; }
  const std::string& serverTime() const { return manifestServerTime; }
  uint32_t pollIntervalSeconds() const { return pollInterval; }
  uint32_t alertPollIntervalSeconds() const { return alertPollInterval; }
  const std::vector<ReleaseFileEntry>& files() const { return releaseFiles; }

 private:
  static void onKey(void* ctx, const char* value, size_t length);
  static void onString(void* ctx, const char* value, size_t length);
  static void onNumber(void* ctx, const char* value, size_t length);
  static void onBool(void* ctx, bool value);
  static void onObjectStart(void* ctx);
  static void onObjectEnd(void* ctx);
  static void onArrayStart(void* ctx);
  static void onArrayEnd(void* ctx);

  void finishFile();

  JsonCallbacks callbacks;
  StreamingJsonParser parser;
  std::string currentKey;
  ReleaseFileEntry currentFile;
  std::vector<ReleaseFileEntry> releaseFiles;
  std::string manifestServerTime;
  FileCallback callback = nullptr;
  void* callbackCtx = nullptr;
  uint32_t releaseVersion = 0;
  uint32_t pollInterval = 0;
  uint32_t alertPollInterval = 0;
  size_t decodedFileCount = 0;
  uint8_t level = 0;
  uint8_t filesArrayLevel = 0;
  bool inFiles = false;
  bool inFile = false;
  bool releaseUnchanged = false;
  bool invalid = false;
};

class ScheduleStreamDecoder {
 public:
  static constexpr size_t MAX_WINDOWS = 32;

  ScheduleStreamDecoder();

  void feed(const char* data, size_t length);
  bool finish() const;

  const std::vector<ScheduleWindow>& windows() const { return scheduleWindows; }
  std::vector<ScheduleWindow> takeWindows() { return std::move(scheduleWindows); }

 private:
  static void onKey(void* ctx, const char* value, size_t length);
  static void onString(void* ctx, const char* value, size_t length);
  static void onBool(void* ctx, bool value);
  static void onObjectStart(void* ctx);
  static void onObjectEnd(void* ctx);
  static void onArrayStart(void* ctx);
  static void onArrayEnd(void* ctx);

  void finishWindow();

  JsonCallbacks callbacks;
  StreamingJsonParser parser;
  std::string currentKey;
  std::string startTime;
  std::string endTime;
  ScheduleWindow currentWindow;
  std::vector<ScheduleWindow> scheduleWindows;
  uint8_t level = 0;
  uint8_t windowsArrayLevel = 0;
  bool inWindows = false;
  bool inWindow = false;
  bool invalid = false;
};

enum class ContentPassMode : uint8_t {
  MEASURE,
  SELECT_UNUSED,
  SELECT_ALL,
};

class ContentStreamDecoder {
 public:
  static constexpr size_t MAX_COPIES = 64;

  ContentStreamDecoder(ContentPassMode mode, const std::vector<int64_t>& usedCopyIds,
                       uint32_t selectionTarget = 0);

  void feed(const char* data, size_t length);
  bool finish() const;
  bool finishAllowEmpty() const;

  uint32_t totalWeight() const { return allWeight; }
  uint32_t unusedWeight() const { return availableWeight; }
  const ContentCopy* selected() const { return hasSelection ? &selectedCopy : nullptr; }
  ContentCopy takeSelected() { return std::move(selectedCopy); }

 private:
  static void onKey(void* ctx, const char* value, size_t length);
  static void onString(void* ctx, const char* value, size_t length);
  static void onNumber(void* ctx, const char* value, size_t length);
  static void onObjectStart(void* ctx);
  static void onObjectEnd(void* ctx);
  static void onArrayStart(void* ctx);
  static void onArrayEnd(void* ctx);

  void finishCopy();
  bool wasUsed(int64_t id) const;

  JsonCallbacks callbacks;
  StreamingJsonParser parser;
  const std::vector<int64_t>& usedIds;
  ContentPassMode passMode;
  std::string currentKey;
  ContentCopy currentCopy;
  ContentCopy selectedCopy;
  uint32_t target = 0;
  uint32_t allWeight = 0;
  uint32_t availableWeight = 0;
  size_t copyCount = 0;
  uint8_t level = 0;
  uint8_t copiesArrayLevel = 0;
  bool inCopies = false;
  bool copiesSeen = false;
  bool inCopy = false;
  bool currentHasText = false;
  bool hasSelection = false;
  bool invalid = false;
};

}  // namespace project_stick
