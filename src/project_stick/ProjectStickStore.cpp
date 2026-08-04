#include "ProjectStickStore.h"

#include <algorithm>
#include <utility>

void ProjectStickStore::toJson(JsonDocument& doc) const {
  doc["device_id"] = deviceId;
  doc["active_version"] = activeVersion;
  doc["previous_version"] = previousVersion;
  doc["poll_interval_seconds"] = pollIntervalSeconds;
  doc["alert_poll_interval_seconds"] = alertPollIntervalSeconds;
  doc["content_refresh_interval_seconds"] = contentRefreshIntervalSeconds;
  doc["is_trading_day"] = tradingDay;
  doc["used_day"] = usedDay;
  if (rotationAnchor.valid) {
    doc["rotation_anchor_day"] = rotationAnchor.day;
    doc["rotation_anchor_second"] = rotationAnchor.secondOfDay;
  }
  if (displayCopyId != 0) {
    doc["display_version"] = displayVersion;
    doc["display_scenario"] = displayScenario;
    doc["display_text"] = displayText;
    doc["display_tone"] = displayTone;
    doc["display_copy_id"] = displayCopyId;
    doc["display_alert"] = displayAlert;
    if (displayAlertUntil.valid) {
      doc["display_alert_until_day"] = displayAlertUntil.day;
      doc["display_alert_until_second"] = displayAlertUntil.secondOfDay;
    }
  }

  JsonArray used = doc["used_copy_ids"].to<JsonArray>();
  for (const int64_t id : usedCopyIds) used.add(id);

  JsonArray alerts = doc["seen_alert_ids"].to<JsonArray>();
  for (const int64_t id : seenAlertIds) alerts.add(id);

  JsonArray events = doc["pending_events"].to<JsonArray>();
  for (const auto& event : pendingEvents) {
    JsonObject obj = events.add<JsonObject>();
    obj["client_event_id"] = event.id;
    obj["event_type"] = event.type;
    if (!event.scenario.empty()) obj["scenario"] = event.scenario;
    if (event.copyId != 0) obj["copy_id"] = event.copyId;
    if (!event.clientTs.empty()) obj["client_ts"] = event.clientTs;
  }
}

bool ProjectStickStore::fromJson(JsonVariantConst doc) {
  deviceId = doc["device_id"] | "";
  activeVersion = doc["active_version"] | 0;
  previousVersion = doc["previous_version"] | 0;
  pollIntervalSeconds = std::clamp<uint32_t>(doc["poll_interval_seconds"] | 300, 30, 86400);
  alertPollIntervalSeconds = std::clamp<uint32_t>(doc["alert_poll_interval_seconds"] | 30, 10, 3600);
  const uint32_t refreshInterval = doc["content_refresh_interval_seconds"] | 600;
  contentRefreshIntervalSeconds =
      refreshInterval == 0 ? 0 : std::clamp<uint32_t>(refreshInterval, 60, 86400);
  tradingDay = doc["is_trading_day"] | false;
  usedDay = doc["used_day"] | 0;
  rotationAnchor.day = doc["rotation_anchor_day"] | 0;
  rotationAnchor.secondOfDay = doc["rotation_anchor_second"] | 0;
  rotationAnchor.valid = rotationAnchor.day != 0 && rotationAnchor.secondOfDay < 86400;
  displayVersion = doc["display_version"] | 0;
  displayScenario = std::string(doc["display_scenario"] | "").substr(0, 64);
  displayText = std::string(doc["display_text"] | "").substr(0, 256);
  displayTone = std::string(doc["display_tone"] | "").substr(0, 32);
  displayCopyId = doc["display_copy_id"] | 0;
  displayAlert = doc["display_alert"] | false;
  displayAlertUntil.day = doc["display_alert_until_day"] | 0;
  displayAlertUntil.secondOfDay = doc["display_alert_until_second"] | 0;
  displayAlertUntil.valid =
      displayAlertUntil.day != 0 && displayAlertUntil.secondOfDay < 86400;
  if (displayVersion != activeVersion || displayScenario.empty() ||
      displayText.empty() || displayCopyId == 0) {
    displayVersion = 0;
    displayScenario.clear();
    displayText.clear();
    displayTone.clear();
    displayCopyId = 0;
    displayAlert = false;
    displayAlertUntil = {};
  }

  usedCopyIds.clear();
  JsonArrayConst used = doc["used_copy_ids"].as<JsonArrayConst>();
  usedCopyIds.reserve(std::min(used.size(), MAX_USED_IDS));
  for (const int64_t id : used) {
    if (usedCopyIds.size() >= MAX_USED_IDS) break;
    usedCopyIds.push_back(id);
  }

  seenAlertIds.clear();
  JsonArrayConst alerts = doc["seen_alert_ids"].as<JsonArrayConst>();
  seenAlertIds.reserve(std::min<size_t>(alerts.size(), 32));
  for (const int64_t id : alerts) {
    if (seenAlertIds.size() >= 32) break;
    seenAlertIds.push_back(id);
  }
  const int64_t legacyLastAlert = doc["last_alert_id"] | 0;
  if (seenAlertIds.empty() && legacyLastAlert != 0) seenAlertIds.push_back(legacyLastAlert);

  pendingEvents.clear();
  JsonArrayConst events = doc["pending_events"].as<JsonArrayConst>();
  pendingEvents.reserve(std::min(events.size(), MAX_PENDING_EVENTS));
  for (JsonObjectConst obj : events) {
    if (pendingEvents.size() >= MAX_PENDING_EVENTS) break;
    ProjectStickEvent event;
    event.id = obj["client_event_id"] | "";
    event.type = obj["event_type"] | "";
    event.scenario = obj["scenario"] | "";
    event.copyId = obj["copy_id"] | 0;
    event.clientTs = obj["client_ts"] | "";
    if (!event.id.empty() && !event.type.empty()) pendingEvents.push_back(std::move(event));
  }
  return true;
}

void ProjectStickStore::markCopyUsed(int64_t day, int64_t id) {
  if (day != usedDay) {
    usedDay = day;
    usedCopyIds.clear();
  }
  if (std::find(usedCopyIds.begin(), usedCopyIds.end(), id) == usedCopyIds.end()) {
    if (usedCopyIds.size() >= MAX_USED_IDS) usedCopyIds.erase(usedCopyIds.begin());
    usedCopyIds.push_back(id);
  }
}

bool ProjectStickStore::hasSeenAlert(int64_t id) const {
  return std::find(seenAlertIds.begin(), seenAlertIds.end(), id) != seenAlertIds.end();
}

void ProjectStickStore::markAlertSeen(int64_t id) {
  if (hasSeenAlert(id)) return;
  if (seenAlertIds.size() >= 32) seenAlertIds.erase(seenAlertIds.begin());
  seenAlertIds.push_back(id);
}

void ProjectStickStore::enqueue(ProjectStickEvent event) {
  if (pendingEvents.size() >= MAX_PENDING_EVENTS) pendingEvents.erase(pendingEvents.begin());
  pendingEvents.push_back(std::move(event));
}
