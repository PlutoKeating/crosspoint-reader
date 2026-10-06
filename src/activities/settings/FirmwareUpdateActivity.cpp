#include "FirmwareUpdateActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/OtaTrial.h"
#include "project_stick/FirmwareInstall.h"
#include "project_stick/FirmwareUpdateState.h"
#include "project_stick/ProjectStickBackgroundSync.h"
#include "project_stick/ProjectStickHost.h"

namespace {
// The specific reason a check failed, with the HTTP status
// for server errors.
const char* checkFailureText(const ProjectStickService::FirmwareOffer& offer, char* buffer, size_t size) {
  using project_stick::NetFailure;
  switch (offer.failure) {
    case NetFailure::Timeout:
      return tr(STR_OTA_FAIL_TIMEOUT);
    case NetFailure::Clock:
      return tr(STR_OTA_FAIL_CLOCK);
    case NetFailure::Network:
      return tr(STR_OTA_FAIL_NETWORK);
    case NetFailure::Memory:
      if (offer.freeHeap > 0) {
        snprintf(buffer, size, tr(STR_OTA_FAIL_MEMORY_KB), static_cast<unsigned>(offer.freeHeap / 1024));
        return buffer;
      }
      return tr(STR_OTA_FAIL_MEMORY);
    case NetFailure::RateLimited:
      // Only a real 429 / Retry-After from the server (2.7.3).
      if (offer.retryAfterSeconds > 0) {
        snprintf(buffer, size, tr(STR_OTA_FAIL_BUSY_SECONDS), static_cast<unsigned>(offer.retryAfterSeconds));
        return buffer;
      }
      return tr(STR_OTA_FAIL_BUSY);
    case NetFailure::Server:
      if (offer.httpStatus > 0) {
        snprintf(buffer, size, "%s (%d)", tr(STR_OTA_FAIL_SERVER), offer.httpStatus);
        return buffer;
      }
      return tr(STR_OTA_FAIL_SERVER);
    default:
      return tr(STR_OTA_CHECK_FAILED);
  }
}

// Why an install stopped (firmware_update error codes from installFirmware).
const char* installFailureText(const char* error) {
  if (strcmp(error, "low_battery") == 0) return tr(STR_OTA_LOW_BATTERY);
  if (strcmp(error, "trial_active") == 0) return tr(STR_OTA_TRIAL_BUSY);
  if (strcmp(error, "device_busy") == 0) return tr(STR_OTA_FAIL_DEVICE_BUSY);
  if (strcmp(error, "download_failed") == 0) return tr(STR_OTA_FAIL_DOWNLOAD);
  if (strcmp(error, "checksum_mismatch") == 0) return tr(STR_OTA_FAIL_CHECKSUM);
  // A file that passed the hash but is not a valid image for the flasher.
  if (strncmp(error, "BAD_", 4) == 0) return tr(STR_OTA_FAIL_CHECKSUM);
  // Image identity verdicts (stick_fw::installVerdictName).
  for (const char* verdict :
       {"NOT_STOCKSTICK_IMAGE", "WRONG_CHIP", "UNSUPPORTED_BOARD", "BELOW_MINIMUM_BUILD", "VERSION_MISMATCH"}) {
    if (strcmp(error, verdict) == 0) return tr(STR_OTA_FAIL_IMAGE);
  }
  return tr(STR_OTA_FAILED);
}
}  // namespace

void FirmwareUpdateActivity::onEnter() {
  Activity::onEnter();
  // Wi-Fi is on demand: this page brings it up while open (the check and the
  // download run on the worker, which waits for the link).
  PROJECT_STICK_HOST.holdWifi(true);
  backgroundSequence = PROJECT_STICK_BACKGROUND_SYNC.latestSequence();
  const auto outcome = ota_trial::pendingOutcome();
  rolledBackNotice = outcome.pending && outcome.rolledBack;
  rolledBackVersion = outcome.version;
  state = firmware_update::snapshot().busy() ? State::Installing : State::Idle;
  requestUpdate();
}

void FirmwareUpdateActivity::onExit() {
  PROJECT_STICK_HOST.holdWifi(false);
  Activity::onExit();
}

void FirmwareUpdateActivity::startCheck() {
  checkFailedOffline = false;
  // A saved network is enough: the host joins it for this page and the worker
  // waits for the link. Only a device with no network at all needs the picker.
  if (WiFi.status() != WL_CONNECTED && !PROJECT_STICK_HOST.wifiNetworkSaved()) {
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
                               checkFailedOffline = true;
                               state = State::Idle;
                               requestUpdate();
                               return;
                             }
                             startCheck();
                           });
    return;
  }
  state = State::Checking;
  offer = {};
  checkDeadline.start(millis(), project_stick::FIRMWARE_CHECK_DEADLINE_MS);
  // A user-initiated check may skip an error backoff (not a server rate limit).
  ProjectStickService::clearBackoffForManualSync();
  awaitingWork = PROJECT_STICK_BACKGROUND_SYNC.requestFirmwareCheck();
  LOG_INF("OTA", "Firmware check %s", awaitingWork ? "queued" : "waiting for the worker");
  requestUpdate();
}

void FirmwareUpdateActivity::startInstall() {
  state = State::Installing;
  firmware_update::reset();
  ProjectStickService::clearBackoffForManualSync();
  awaitingWork = PROJECT_STICK_BACKGROUND_SYNC.requestFirmwareInstall(offer.target);
  requestUpdate();
}

void FirmwareUpdateActivity::pollBackground() {
  // The worker handles one job at a time; retry queuing until it is free.
  if (!awaitingWork) {
    if (state == State::Checking) awaitingWork = PROJECT_STICK_BACKGROUND_SYNC.requestFirmwareCheck();
    if (state == State::Installing && !firmware_update::snapshot().busy() && !offer.target.url.empty()) {
      awaitingWork = PROJECT_STICK_BACKGROUND_SYNC.requestFirmwareInstall(offer.target);
    }
  }
  ProjectStickBackgroundSync::Result result;
  while (PROJECT_STICK_BACKGROUND_SYNC.takeResult(backgroundSequence, result)) {
    using Kind = ProjectStickBackgroundSync::WorkKind;
    if (result.kind == Kind::FirmwareCheck && state == State::Checking) {
      offer = result.firmware;
      state = State::Result;
      awaitingWork = false;
      checkDeadline.stop();
      requestUpdate();
    } else if (result.kind == Kind::FirmwareInstall && state == State::Installing) {
      // A successful install restarts the device, so reaching here means the
      // install stopped early (see firmware_update for the reason).
      offer.status = result.firmware.status;
      state = State::Result;
      awaitingWork = false;
      requestUpdate();
    }
  }
  if (state == State::Checking && checkDeadline.expired(millis())) {
    // The worker is stuck behind a long job or the request outlived every
    // timeout: say so instead of spinning. A late result is ignored.
    LOG_ERR("OTA", "Firmware check timed out (worker %s)", awaitingWork ? "busy with it" : "never free");
    offer = {};
    offer.failure = project_stick::NetFailure::Timeout;
    state = State::Result;
    awaitingWork = false;
    checkDeadline.stop();
    requestUpdate();
  }
  const auto progress = firmware_update::snapshot();
  if (progress.generation != renderedProgressGeneration) {
    renderedProgressGeneration = progress.generation;
    if (progress.busy()) state = State::Installing;
    requestUpdate();
  }
}

void FirmwareUpdateActivity::loop() {
  pollBackground();
  if (state == State::Confirming) return;
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) backPressSeen = true;
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) confirmPressSeen = true;
  if (backPressSeen && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Leaving never cancels work: the worker keeps downloading/installing.
    finish();
    return;
  }
  if (!confirmPressSeen || !mappedInput.wasReleased(MappedInputManager::Button::Confirm)) return;
  confirmPressSeen = false;
  using Status = ProjectStickService::FirmwareOffer::Status;
  if (state == State::Result && offer.status == Status::InstallFailed && !offer.target.url.empty()) {
    startInstall();  // retry the same image; a partial download resumes
  } else if (state == State::Idle || (state == State::Result && offer.status != Status::UpdateAvailable)) {
    startCheck();
  } else if (state == State::Result) {
    state = State::Confirming;
    char heading[64];
    snprintf(heading, sizeof(heading), "%s %s", tr(STR_OTA_AVAILABLE), offer.target.version.c_str());
    startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, offer.notes),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) {
                               state = State::Result;
                               requestUpdate();
                               return;
                             }
                             startInstall();
                           });
  }
}

void FirmwareUpdateActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int side = metrics.contentSidePadding;
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_OTA_TITLE));

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;
  char line[96];
  snprintf(line, sizeof(line), "%s  %s (%u)", tr(STR_OTA_CURRENT), CROSSPOINT_VERSION,
           static_cast<unsigned>(firmware_install::runningBuild()));
  renderer.drawText(UI_10_FONT_ID, side, y, line, true);
  y += lineHeight * 2;

  if (rolledBackNotice) {
    const Rect notice{side, y, pageWidth - side * 2, lineHeight * 3};
    UITheme::drawCenteredWrappedText(renderer, notice, UI_10_FONT_ID, tr(STR_OTA_ROLLED_BACK), 3, true,
                                     EpdFontFamily::BOLD, UITheme::TextVerticalAlignment::TOP);
    y += lineHeight * 4;
  }

  const auto progress = firmware_update::snapshot();
  const char* message = nullptr;
  const char* confirmLabel = tr(STR_OTA_CHECK);
  using Status = ProjectStickService::FirmwareOffer::Status;
  switch (state) {
    case State::Idle:
      message = checkFailedOffline ? tr(STR_OTA_NEED_WIFI) : nullptr;
      break;
    case State::Checking:
      message = tr(STR_OTA_CHECKING);
      confirmLabel = "";
      break;
    case State::Confirming:
      break;
    case State::Result:
      if (offer.status == Status::UpdateAvailable) {
        snprintf(line, sizeof(line), "%s  %s", tr(STR_OTA_LATEST), offer.target.version.c_str());
        renderer.drawText(UI_10_FONT_ID, side, y, line, true, EpdFontFamily::BOLD);
        y += lineHeight * 2;
        if (!offer.notes.empty()) {
          const int notesPitch = renderer.getTextLineHeight(UI_10_FONT_ID, offer.notes.c_str());
          UITheme::drawCenteredWrappedText(renderer, Rect{side, y, pageWidth - side * 2, notesPitch * 8}, UI_10_FONT_ID,
                                           offer.notes.c_str(), 8, true, EpdFontFamily::REGULAR,
                                           UITheme::TextVerticalAlignment::TOP);
        }
        confirmLabel = tr(STR_OTA_INSTALL);
      } else if (offer.status == Status::UpToDate) {
        message = tr(STR_OTA_UP_TO_DATE);
      } else if (offer.status == Status::InstallFailed) {
        message = installFailureText(progress.error);
        // The offer is still valid: the same key retries (a download resumes).
        if (!offer.target.url.empty()) confirmLabel = tr(STR_OTA_INSTALL);
      } else {
        message = checkFailureText(offer, line, sizeof(line));
      }
      break;
    case State::Installing: {
      confirmLabel = "";
      switch (progress.phase) {
        case firmware_update::Phase::Verifying:
          message = tr(STR_OTA_VERIFYING);
          break;
        case firmware_update::Phase::Installing:
          message = tr(STR_OTA_INSTALLING);
          break;
        case firmware_update::Phase::Restarting:
          message = tr(STR_OTA_RESTARTING);
          break;
        case firmware_update::Phase::Failed:
          message = installFailureText(progress.error);
          confirmLabel = tr(STR_OTA_CHECK);
          break;
        default:
          message = tr(STR_OTA_DOWNLOADING);
          break;
      }
      if (progress.busy() && progress.total > 0) {
        GUI.drawProgressBar(renderer, Rect{side, y + lineHeight * 2, pageWidth - side * 2, metrics.progressBarHeight},
                            progress.percent(), 100);
      }
      break;
    }
  }
  if (message) {
    const int messagePitch = renderer.getTextLineHeight(UI_10_FONT_ID, message, EpdFontFamily::BOLD);
    UITheme::drawCenteredWrappedText(renderer, Rect{side, y, pageWidth - side * 2, messagePitch * 4}, UI_10_FONT_ID,
                                     message, 4, true, EpdFontFamily::BOLD, UITheme::TextVerticalAlignment::TOP);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
