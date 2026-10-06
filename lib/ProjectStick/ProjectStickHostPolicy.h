#pragma once

#include <cstdint>

// Pure decisions of ProjectStickHost (src/project_stick), the page-independent
// part of the StockStick product.

// What the host may do with the page that is open when a phone delivers
// content or starts a firmware update over BLE. Both must be seen: content is
// shown (and its "displayed" receipt produced) by the StockStick page, and a
// firmware update owns the screen until the restart.
enum class StickTakeover : uint8_t {
  Allowed,       // any ordinary page: switch to the StockStick page
  StickPage,     // the StockStick page itself: nothing to do
  FirmwarePage,  // shows firmware progress itself; incoming content still switches
  Never,         // boot, sleep, crash report, SD flashing, a check or install in progress
};

namespace project_stick {

// True when the host should switch to the StockStick page now.
// `firmwareUpdating`: a download/verify/flash is running (BLE `ota` or Settings).
// `contentIncoming`: a BLE transfer is receiving or waiting to be displayed.
inline bool shouldShowStickPage(StickTakeover page, bool firmwareUpdating, bool contentIncoming) {
  if (page == StickTakeover::StickPage || page == StickTakeover::Never) return false;
  // An update outranks content: the transfer is refused while it runs.
  if (firmwareUpdating) return page == StickTakeover::Allowed;
  return contentIncoming;
}

}  // namespace project_stick
