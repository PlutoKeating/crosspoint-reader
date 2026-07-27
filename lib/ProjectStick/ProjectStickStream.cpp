#include "ProjectStickStream.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace project_stick {

ReleaseManifestDecoder::ReleaseManifestDecoder(FileCallback fileCallback, void* callbackContext)
    : callbacks{this, onKey, onString, onNumber, onBool, nullptr, onObjectStart, onObjectEnd, onArrayStart,
                onArrayEnd},
      parser(callbacks),
      callback(fileCallback),
      callbackCtx(callbackContext) {
  if (!callback) releaseFiles.reserve(MAX_FILES);
}

void ReleaseManifestDecoder::feed(const char* data, size_t length) {
  if (!invalid) parser.feed(data, length);
}

bool ReleaseManifestDecoder::finish() const {
  return !invalid && !parser.hasError() && level == 0 && !inFile && releaseVersion != 0 &&
         (releaseUnchanged || decodedFileCount != 0);
}

void ReleaseManifestDecoder::onKey(void* ctx, const char* value, size_t length) {
  auto* self = static_cast<ReleaseManifestDecoder*>(ctx);
  self->currentKey.assign(value, length);
}

void ReleaseManifestDecoder::onString(void* ctx, const char* value, size_t length) {
  auto* self = static_cast<ReleaseManifestDecoder*>(ctx);
  if (!self->inFile) {
    if (!self->inFiles && self->currentKey == "server_time") {
      self->manifestServerTime.assign(value, length);
    }
    return;
  }
  if (self->currentKey == "path") {
    self->currentFile.path.assign(value, length);
  } else if (self->currentKey == "sha256") {
    self->currentFile.sha256.assign(value, length);
  }
}

void ReleaseManifestDecoder::onNumber(void* ctx, const char* value, size_t /*length*/) {
  auto* self = static_cast<ReleaseManifestDecoder*>(ctx);
  const unsigned long parsed = std::strtoul(value, nullptr, 10);
  if (self->inFile && self->currentKey == "size") {
    self->currentFile.size = static_cast<size_t>(parsed);
  } else if (!self->inFiles && self->currentKey == "version") {
    self->releaseVersion = static_cast<uint32_t>(parsed);
  } else if (!self->inFiles && self->currentKey == "poll_interval_seconds") {
    self->pollInterval = static_cast<uint32_t>(parsed);
  } else if (!self->inFiles && self->currentKey == "alert_poll_interval_seconds") {
    self->alertPollInterval = static_cast<uint32_t>(parsed);
  }
}

void ReleaseManifestDecoder::onBool(void* ctx, bool value) {
  auto* self = static_cast<ReleaseManifestDecoder*>(ctx);
  if (!self->inFiles && self->currentKey == "unchanged") self->releaseUnchanged = value;
}

void ReleaseManifestDecoder::onObjectStart(void* ctx) {
  auto* self = static_cast<ReleaseManifestDecoder*>(ctx);
  ++self->level;
  if (self->inFiles && self->level == self->filesArrayLevel + 1) {
    self->currentFile = {};
    self->inFile = true;
  }
}

void ReleaseManifestDecoder::onObjectEnd(void* ctx) {
  auto* self = static_cast<ReleaseManifestDecoder*>(ctx);
  if (self->inFile && self->level == self->filesArrayLevel + 1) self->finishFile();
  if (self->level == 0) {
    self->invalid = true;
  } else {
    --self->level;
  }
}

void ReleaseManifestDecoder::onArrayStart(void* ctx) {
  auto* self = static_cast<ReleaseManifestDecoder*>(ctx);
  ++self->level;
  if (!self->inFiles && self->currentKey == "files") {
    self->inFiles = true;
    self->filesArrayLevel = self->level;
  }
}

void ReleaseManifestDecoder::onArrayEnd(void* ctx) {
  auto* self = static_cast<ReleaseManifestDecoder*>(ctx);
  if (self->inFiles && self->level == self->filesArrayLevel) self->inFiles = false;
  if (self->level == 0) {
    self->invalid = true;
  } else {
    --self->level;
  }
}

void ReleaseManifestDecoder::finishFile() {
  inFile = false;
  if (decodedFileCount >= MAX_FILES || currentFile.path.empty() || currentFile.sha256.empty() ||
      currentFile.size == 0) {
    invalid = true;
    return;
  }
  ++decodedFileCount;
  if (callback) {
    if (!callback(callbackCtx, currentFile)) invalid = true;
  } else {
    releaseFiles.push_back(std::move(currentFile));
  }
}

ScheduleStreamDecoder::ScheduleStreamDecoder()
    : callbacks{this, onKey, onString, nullptr, onBool, nullptr, onObjectStart, onObjectEnd, onArrayStart,
                onArrayEnd},
      parser(callbacks) {
  scheduleWindows.reserve(MAX_WINDOWS);
}

void ScheduleStreamDecoder::feed(const char* data, size_t length) {
  if (!invalid) parser.feed(data, length);
}

bool ScheduleStreamDecoder::finish() const {
  return !invalid && !parser.hasError() && level == 0 && !inWindow && !scheduleWindows.empty();
}

void ScheduleStreamDecoder::onKey(void* ctx, const char* value, size_t length) {
  auto* self = static_cast<ScheduleStreamDecoder*>(ctx);
  self->currentKey.assign(value, length);
}

void ScheduleStreamDecoder::onString(void* ctx, const char* value, size_t length) {
  auto* self = static_cast<ScheduleStreamDecoder*>(ctx);
  if (!self->inWindow) return;
  if (self->currentKey == "scenario") {
    self->currentWindow.scenario.assign(value, length);
  } else if (self->currentKey == "start") {
    self->startTime.assign(value, length);
  } else if (self->currentKey == "end") {
    self->endTime.assign(value, length);
  }
}

void ScheduleStreamDecoder::onBool(void* ctx, bool value) {
  auto* self = static_cast<ScheduleStreamDecoder*>(ctx);
  if (!self->inWindow) return;
  if (self->currentKey == "enabled") {
    self->currentWindow.enabled = value;
  } else if (self->currentKey == "trading_day_only") {
    self->currentWindow.tradingDayOnly = value;
  } else if (self->currentKey == "all_day") {
    self->currentWindow.allDay = value;
  }
}

void ScheduleStreamDecoder::onObjectStart(void* ctx) {
  auto* self = static_cast<ScheduleStreamDecoder*>(ctx);
  ++self->level;
  if (self->inWindows && self->level == self->windowsArrayLevel + 1) {
    self->currentWindow = {};
    self->currentWindow.enabled = true;
    self->startTime.clear();
    self->endTime.clear();
    self->inWindow = true;
  }
}

void ScheduleStreamDecoder::onObjectEnd(void* ctx) {
  auto* self = static_cast<ScheduleStreamDecoder*>(ctx);
  if (self->inWindow && self->level == self->windowsArrayLevel + 1) self->finishWindow();
  if (self->level == 0) {
    self->invalid = true;
  } else {
    --self->level;
  }
}

void ScheduleStreamDecoder::onArrayStart(void* ctx) {
  auto* self = static_cast<ScheduleStreamDecoder*>(ctx);
  ++self->level;
  if (!self->inWindows && self->currentKey == "windows") {
    self->inWindows = true;
    self->windowsArrayLevel = self->level;
  }
}

void ScheduleStreamDecoder::onArrayEnd(void* ctx) {
  auto* self = static_cast<ScheduleStreamDecoder*>(ctx);
  if (self->inWindows && self->level == self->windowsArrayLevel) self->inWindows = false;
  if (self->level == 0) {
    self->invalid = true;
  } else {
    --self->level;
  }
}

void ScheduleStreamDecoder::finishWindow() {
  inWindow = false;
  if (scheduleWindows.size() >= MAX_WINDOWS || currentWindow.scenario.empty()) {
    invalid = true;
    return;
  }
  if (!currentWindow.allDay &&
      (!parseClockMinute(startTime.c_str(), currentWindow.startMinute) ||
       !parseClockMinute(endTime.c_str(), currentWindow.endMinute))) {
    invalid = true;
    return;
  }
  scheduleWindows.push_back(std::move(currentWindow));
}

ContentStreamDecoder::ContentStreamDecoder(ContentPassMode mode, const std::vector<int64_t>& usedCopyIds,
                                           uint32_t selectionTarget)
    : callbacks{this, onKey, onString, onNumber, nullptr, nullptr, onObjectStart, onObjectEnd, onArrayStart,
                onArrayEnd},
      parser(callbacks),
      usedIds(usedCopyIds),
      passMode(mode),
      target(selectionTarget) {}

void ContentStreamDecoder::feed(const char* data, size_t length) {
  if (!invalid) parser.feed(data, length);
}

bool ContentStreamDecoder::finish() const {
  const bool passResult = passMode == ContentPassMode::MEASURE ? allWeight != 0 : hasSelection;
  return !invalid && !parser.hasError() && level == 0 && !inCopy && copyCount != 0 && passResult;
}

void ContentStreamDecoder::onKey(void* ctx, const char* value, size_t length) {
  auto* self = static_cast<ContentStreamDecoder*>(ctx);
  self->currentKey.assign(value, length);
}

void ContentStreamDecoder::onString(void* ctx, const char* value, size_t length) {
  auto* self = static_cast<ContentStreamDecoder*>(ctx);
  if (!self->inCopy || self->hasSelection) return;
  if (self->currentKey == "text") self->currentHasText = length != 0;
  if (self->passMode == ContentPassMode::MEASURE) return;
  if (self->currentKey == "text") {
    self->currentCopy.text.assign(value, length);
  } else if (self->currentKey == "tone") {
    self->currentCopy.tone.assign(value, length);
  }
}

void ContentStreamDecoder::onNumber(void* ctx, const char* value, size_t /*length*/) {
  auto* self = static_cast<ContentStreamDecoder*>(ctx);
  if (!self->inCopy) return;
  if (self->currentKey == "id") {
    self->currentCopy.id = std::strtoll(value, nullptr, 10);
  } else if (self->currentKey == "weight") {
    const unsigned long parsed = std::strtoul(value, nullptr, 10);
    self->currentCopy.weight = static_cast<uint16_t>(std::clamp<unsigned long>(parsed, 1, 1000));
  }
}

void ContentStreamDecoder::onObjectStart(void* ctx) {
  auto* self = static_cast<ContentStreamDecoder*>(ctx);
  ++self->level;
  if (self->inCopies && self->level == self->copiesArrayLevel + 1) {
    self->currentCopy = {};
    self->currentCopy.weight = 1;
    self->currentHasText = false;
    self->inCopy = true;
  }
}

void ContentStreamDecoder::onObjectEnd(void* ctx) {
  auto* self = static_cast<ContentStreamDecoder*>(ctx);
  if (self->inCopy && self->level == self->copiesArrayLevel + 1) self->finishCopy();
  if (self->level == 0) {
    self->invalid = true;
  } else {
    --self->level;
  }
}

void ContentStreamDecoder::onArrayStart(void* ctx) {
  auto* self = static_cast<ContentStreamDecoder*>(ctx);
  ++self->level;
  if (!self->inCopies && self->currentKey == "copies") {
    self->inCopies = true;
    self->copiesArrayLevel = self->level;
  }
}

void ContentStreamDecoder::onArrayEnd(void* ctx) {
  auto* self = static_cast<ContentStreamDecoder*>(ctx);
  if (self->inCopies && self->level == self->copiesArrayLevel) self->inCopies = false;
  if (self->level == 0) {
    self->invalid = true;
  } else {
    --self->level;
  }
}

void ContentStreamDecoder::finishCopy() {
  inCopy = false;
  if (copyCount >= MAX_COPIES) {
    invalid = true;
    return;
  }
  ++copyCount;
  if (currentCopy.id == 0 || !currentHasText) return;

  const uint32_t weight = currentCopy.weight;
  const bool used = wasUsed(currentCopy.id);
  if (passMode == ContentPassMode::MEASURE) {
    allWeight += weight;
    if (!used) availableWeight += weight;
    return;
  }
  if (hasSelection || currentCopy.text.empty() ||
      (passMode == ContentPassMode::SELECT_UNUSED && used)) {
    return;
  }
  if (target < weight) {
    selectedCopy = std::move(currentCopy);
    hasSelection = true;
  } else {
    target -= weight;
  }
}

bool ContentStreamDecoder::wasUsed(int64_t id) const {
  return std::find(usedIds.begin(), usedIds.end(), id) != usedIds.end();
}

}  // namespace project_stick
