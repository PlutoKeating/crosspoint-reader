#include "ProjectStickBackgroundSync.h"

#include <Logging.h>

ProjectStickBackgroundSync& ProjectStickBackgroundSync::getInstance() {
  static ProjectStickBackgroundSync instance;
  return instance;
}

void ProjectStickBackgroundSync::begin() {
  taskENTER_CRITICAL(&stateMux);
  const bool alreadyStarted = started;
  if (!started) started = true;
  taskEXIT_CRITICAL(&stateMux);
  if (alreadyStarted) return;

  // The UI-side service has already loaded the shared store. This second
  // instance owns all cloud I/O and keeps its network/display state off the UI
  // task.
  service.begin();
  const BaseType_t created = xTaskCreate(&taskTrampoline, "ProjectStickSync", 8192, this, 1, &taskHandle);
  if (created != pdTRUE || taskHandle == nullptr) {
    LOG_ERR("STICK", "Could not create background sync task");
    taskENTER_CRITICAL(&stateMux);
    started = false;
    taskEXIT_CRITICAL(&stateMux);
  }
}

bool ProjectStickBackgroundSync::requestSync(bool registerFirst) { return queue(WorkKind::Sync, registerFirst); }

bool ProjectStickBackgroundSync::requestStudioPoll() { return queue(WorkKind::StudioPoll, false); }

bool ProjectStickBackgroundSync::requestAlertPoll() { return queue(WorkKind::AlertPoll, false); }

bool ProjectStickBackgroundSync::requestFirmwareCheck() { return queue(WorkKind::FirmwareCheck, false); }

bool ProjectStickBackgroundSync::requestFirmwareInstall(const std::string& firmwareId) {
  return queue(WorkKind::FirmwareRequest, false, firmwareId);
}

bool ProjectStickBackgroundSync::queue(WorkKind kind, bool registerFirst, const std::string& firmwareId) {
  // Copied outside the spinlock: std::string may allocate.
  std::string id = firmwareId;
  taskENTER_CRITICAL(&stateMux);
  const bool accepted = started && taskHandle != nullptr && gate.tryQueue();
  if (accepted) {
    pendingKind = kind;
    pendingRegisterFirst = registerFirst;
    pendingFirmwareId.swap(id);
  }
  taskEXIT_CRITICAL(&stateMux);
  if (accepted) xTaskNotify(taskHandle, 1, eIncrement);
  return accepted;
}

bool ProjectStickBackgroundSync::busy() const {
  taskENTER_CRITICAL(&stateMux);
  const bool value = gate.busy();
  taskEXIT_CRITICAL(&stateMux);
  return value;
}

uint32_t ProjectStickBackgroundSync::latestSequence() const {
  std::lock_guard<std::mutex> lock(resultMutex);
  return latestResult.sequence;
}

bool ProjectStickBackgroundSync::takeResult(uint32_t& lastSequence, Result& result) const {
  std::lock_guard<std::mutex> lock(resultMutex);
  if (latestResult.sequence == lastSequence) return false;
  result = latestResult;
  lastSequence = latestResult.sequence;
  return true;
}

void ProjectStickBackgroundSync::taskTrampoline(void* context) {
  static_cast<ProjectStickBackgroundSync*>(context)->taskLoop();
}

void ProjectStickBackgroundSync::taskLoop() {
  // Studio polls keep their TLS connection warm; drop it once polling pauses
  // (activity left, Wi-Fi gone, BLE session) so the heap is not held.
  constexpr TickType_t IDLE_CLOSE_TICKS = pdMS_TO_TICKS(45000);
  while (true) {
    if (ulTaskNotifyTake(pdTRUE, IDLE_CLOSE_TICKS) == 0) {
      service.closeIdleConnection();
      continue;
    }

    WorkKind kind = WorkKind::None;
    bool registerFirst = false;
    std::string firmwareId;
    taskENTER_CRITICAL(&stateMux);
    if (gate.begin()) {
      kind = pendingKind;
      registerFirst = pendingRegisterFirst;
      firmwareId.swap(pendingFirmwareId);
      pendingKind = WorkKind::None;
      pendingRegisterFirst = false;
    }
    taskEXIT_CRITICAL(&stateMux);
    if (kind == WorkKind::None) continue;

    Result completed;
    completed.kind = kind;
    if (kind == WorkKind::Sync) {
      completed.syncReport = service.sync(registerFirst);
    } else if (kind == WorkKind::StudioPoll) {
      service.syncStudio();
    } else if (kind == WorkKind::FirmwareCheck) {
      completed.firmware = service.checkFirmware();
    } else if (kind == WorkKind::FirmwareRequest) {
      completed.firmware = service.requestFirmware(firmwareId);
    } else {
      completed.alertReceived = service.pollAlerts();
      if (completed.alertReceived) completed.alertDisplay = service.displaySnapshot();
    }

    {
      std::lock_guard<std::mutex> lock(resultMutex);
      completed.sequence = latestResult.sequence + 1;
      latestResult = std::move(completed);
    }

    taskENTER_CRITICAL(&stateMux);
    gate.complete();
    taskEXIT_CRITICAL(&stateMux);
  }
}
