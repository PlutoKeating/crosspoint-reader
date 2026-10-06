#include "ProjectStickBackgroundSync.h"
#include "FrameStore.h"
#include "StudioFrame.h"
#include "StudioBluetooth.h"

#include <Logging.h>

ProjectStickBackgroundSync& ProjectStickBackgroundSync::getInstance() {
  static ProjectStickBackgroundSync instance;
  return instance;
}

// Called once from setup(), before Wi-Fi or NimBLE allocate. The task stack
// and TCB are static, so the worker always exists: a lazily created task could
// fail on a fragmented heap and leave every cloud request (and the firmware
// check screen) waiting forever.
void ProjectStickBackgroundSync::begin() {
  taskENTER_CRITICAL(&stateMux);
  const bool alreadyStarted = started;
  started = true;
  taskEXIT_CRITICAL(&stateMux);
  if (alreadyStarted) return;

  // This instance owns all cloud I/O and keeps its network state off the UI
  // task; it also loads the shared store first.
  service.begin();
#ifdef SIMULATOR
  TaskHandle_t handle = nullptr;
  xTaskCreate(&taskTrampoline, TASK_NAME, TASK_STACK_BYTES, this, 1, &handle);
#else
  static StackType_t stack[TASK_STACK_BYTES];
  static StaticTask_t taskBuffer;
  TaskHandle_t handle =
      xTaskCreateStatic(&taskTrampoline, TASK_NAME, TASK_STACK_BYTES, this, 1, stack, &taskBuffer);
#endif
  if (handle == nullptr) LOG_ERR("STICK", "Background sync task was not created");
  taskENTER_CRITICAL(&stateMux);
  taskHandle = handle;
  taskEXIT_CRITICAL(&stateMux);
  // BLE transfer work (inflate, SD writes) runs on this task too: it is idle
  // while a phone is connected, and its 8 KB static stack already carries
  // TLS. 2.7.0's separate 6 KB writer task is gone (2.7.2, memory).
  studio_ble::setPumpTask(handle);
}

bool ProjectStickBackgroundSync::requestSync() { return queue(WorkKind::Sync); }

TaskHandle_t ProjectStickBackgroundSync::handle() const {
  taskENTER_CRITICAL(&stateMux);
  TaskHandle_t value = taskHandle;
  taskEXIT_CRITICAL(&stateMux);
  return value;
}

bool ProjectStickBackgroundSync::requestAlertPoll() { return queue(WorkKind::AlertPoll); }

bool ProjectStickBackgroundSync::requestFirmwareCheck() { return queue(WorkKind::FirmwareCheck); }

bool ProjectStickBackgroundSync::requestFirmwareInstall(const ProjectStickService::FirmwareTarget& target) {
  return queue(WorkKind::FirmwareInstall, target);
}

bool ProjectStickBackgroundSync::queue(WorkKind kind, const ProjectStickService::FirmwareTarget& target) {
  // Copied outside the spinlock: std::string may allocate.
  ProjectStickService::FirmwareTarget copy = target;
  taskENTER_CRITICAL(&stateMux);
  const bool accepted = started && taskHandle != nullptr && gate.tryQueue();
  if (accepted) {
    pendingKind = kind;
    std::swap(pendingTarget, copy);
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

ProjectStickBackgroundSync::WorkKind ProjectStickBackgroundSync::runningKind() const {
  taskENTER_CRITICAL(&stateMux);
  const WorkKind value = activeKind;
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
  // Housekeeping between jobs: the card usage scan (whole FAT, seconds) runs
  // here, never on the UI loop, and only while no phone is connected.
  constexpr uint32_t FIRST_HOUSEKEEPING_MS = 20UL * 1000UL, HOUSEKEEPING_MS = 10UL * 60UL * 1000UL;
  uint32_t housekeepingWaitMs = FIRST_HOUSEKEEPING_MS;
  while (true) {
    const bool woken = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(housekeepingWaitMs)) != 0;
    // Queued BLE transfer work first: the phone is waiting for PROGRESS.
    studio_ble::pump();
    if (!woken) {
      if (!StudioFrame::instance().busy() && !studio_ble::connected()) {
        // The transfer areas are created once, here (2.7.10): seconds of
        // cluster scanning that must not run while a phone sends.
        frame_store::prepare();
        StudioFrame::refreshStorageUsage();
        housekeepingWaitMs = HOUSEKEEPING_MS;
      } else {
        housekeepingWaitMs = FIRST_HOUSEKEEPING_MS;  // try again once the phone is gone
      }
      continue;
    }

    WorkKind kind = WorkKind::None;
    ProjectStickService::FirmwareTarget target;
    taskENTER_CRITICAL(&stateMux);
    if (gate.begin()) {
      kind = pendingKind;
      std::swap(target, pendingTarget);
      pendingKind = WorkKind::None;
      activeKind = kind;
    }
    taskEXIT_CRITICAL(&stateMux);
    if (kind == WorkKind::None) continue;

    Result completed;
    completed.kind = kind;
    // User-started jobs (Settings > Firmware update, a BLE `ota`) disconnect a
    // phone at once and are not held by the device's own error backoff.
    if (kind == WorkKind::Sync) {
      service.beginJob("sync", false);
      completed.syncReport = service.sync();
    } else if (kind == WorkKind::FirmwareCheck) {
      service.beginJob("firmware_check", true);
      completed.firmware = service.checkFirmware();
    } else if (kind == WorkKind::FirmwareInstall) {
      service.beginJob("firmware_install", true);
      // Returns only when the install stopped (a success restarts).
      service.installFirmware(target);
      completed.firmware.status = ProjectStickService::FirmwareOffer::Status::InstallFailed;
      completed.firmware.target = std::move(target);
    } else {
      service.beginJob("alerts", false);
      completed.alertReceived = service.pollAlerts();
      if (completed.alertReceived) completed.alertDisplay = service.displaySnapshot();
    }

    // TLS and the flash writer run on this 8 KB stack; the high-water mark
    // shows how much of it a job actually used.
    LOG_INF("STICK", "Worker job %u done (stack free %u B)", (unsigned)kind,
            (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    {
      std::lock_guard<std::mutex> lock(resultMutex);
      completed.sequence = latestResult.sequence + 1;
      latestResult = std::move(completed);
    }

    taskENTER_CRITICAL(&stateMux);
    gate.complete();
    activeKind = WorkKind::None;
    taskEXIT_CRITICAL(&stateMux);
    studio_ble::pump();  // chunks that queued up behind the job
  }
}
