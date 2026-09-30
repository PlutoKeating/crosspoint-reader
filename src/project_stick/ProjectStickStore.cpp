#include "ProjectStickStore.h"

#include <algorithm>
#include <utility>

void ProjectStickStore::toJson(JsonDocument& doc) const {
  doc["device_id"] = deviceId;
  if (!deviceToken.empty()) doc["device_token"] = deviceToken;
  doc["bound"] = bound;
  doc["owner_id"] = ownerId;
  doc["poll_interval_seconds"] = pollIntervalSeconds;
  doc["alert_poll_interval_seconds"] = alertPollIntervalSeconds;
  doc["is_trading_day"] = tradingDay;
  if (alertUntil.valid) {
    doc["alert_until_day"] = alertUntil.day;
    doc["alert_until_second"] = alertUntil.secondOfDay;
  }

  JsonArray alerts = doc["seen_alert_ids"].to<JsonArray>();
  for (const int64_t id : seenAlertIds) alerts.add(id);

  JsonArray events = doc["pending_events"].to<JsonArray>();
  for (const auto& event : pendingEvents) {
    JsonObject obj = events.add<JsonObject>();
    obj["client_event_id"] = event.id;
    obj["event_type"] = event.type;
    if (!event.clientTs.empty()) obj["client_ts"] = event.clientTs;
    if (!event.studioTask.empty()) {
      obj["studio_task"] = event.studioTask;
      obj["studio_card"] = event.studioCard;
    }
    if (!event.detail.empty()) obj["detail"] = event.detail;
  }
}

bool ProjectStickStore::fromJson(JsonVariantConst doc) {
  deviceId = doc["device_id"] | "";
  deviceToken = std::string(doc["device_token"] | "").substr(0, 96);
  bound = doc["bound"] | false;
  ownerId = doc["owner_id"] | "";
  pollIntervalSeconds = std::clamp<uint32_t>(doc["poll_interval_seconds"] | 300, 30, 86400);
  alertPollIntervalSeconds = std::clamp<uint32_t>(doc["alert_poll_interval_seconds"] | 30, 10, 3600);
  tradingDay = doc["is_trading_day"] | false;
  alertUntil.day = doc["alert_until_day"] | 0;
  alertUntil.secondOfDay = doc["alert_until_second"] | 0;
  alertUntil.valid = alertUntil.day != 0 && alertUntil.secondOfDay < 86400;

  seenAlertIds.clear();
  JsonArrayConst alerts = doc["seen_alert_ids"].as<JsonArrayConst>();
  seenAlertIds.reserve(std::min(alerts.size(), MAX_SEEN_ALERTS));
  for (const int64_t id : alerts) {
    if (seenAlertIds.size() >= MAX_SEEN_ALERTS) break;
    seenAlertIds.push_back(id);
  }

  pendingEvents.clear();
  JsonArrayConst events = doc["pending_events"].as<JsonArrayConst>();
  pendingEvents.reserve(std::min(events.size(), MAX_PENDING_EVENTS));
  for (JsonObjectConst obj : events) {
    if (pendingEvents.size() >= MAX_PENDING_EVENTS) break;
    ProjectStickEvent event;
    event.id = obj["client_event_id"] | "";
    event.type = obj["event_type"] | "";
    event.clientTs = obj["client_ts"] | "";
    event.studioTask = obj["studio_task"] | "";
    event.studioCard = obj["studio_card"] | "";
    event.detail = obj["detail"] | "";
    if (!event.id.empty() && !event.type.empty()) pendingEvents.push_back(std::move(event));
  }
  return true;
}

bool ProjectStickStore::hasSeenAlert(int64_t id) const {
  return std::find(seenAlertIds.begin(), seenAlertIds.end(), id) != seenAlertIds.end();
}

void ProjectStickStore::markAlertSeen(int64_t id) {
  if (hasSeenAlert(id)) return;
  if (seenAlertIds.size() >= MAX_SEEN_ALERTS) seenAlertIds.erase(seenAlertIds.begin());
  seenAlertIds.push_back(id);
}

void ProjectStickStore::enqueue(ProjectStickEvent event) {
  if (pendingEvents.size() >= MAX_PENDING_EVENTS) pendingEvents.erase(pendingEvents.begin());
  pendingEvents.push_back(std::move(event));
}
