#pragma once
#include <FirmwareTransfer.h>
#include <HalStorage.h>

#include <cstdint>
#include <string>

// Device side of firmware over BLE (op fw4, Project.StockStick
// docs/product/BLE-TRANSFER-V4.md 「固件经蓝牙传输」). Runs on the pump task
// (the background sync worker) only: every block is inflated straight into
// /.crosspoint/studio/firmware.tmp, the file the on-device Wi-Fi download
// also uses, and a checkpoint in firmware.meta lets a reconnecting phone
// resume at the last completed block. Nothing larger than one compressed
// block is staged, and that on the card.
class FirmwareReceiver final : public firmware_v4::Io {
 public:
  static FirmwareReceiver& instance();

  // Opens firmware.tmp for an image of `size` bytes with hash `sha256`
  // (lowercase hex), resuming a partial of the same image. Sets the stream
  // offset the phone continues from. False with error() on a failure.
  bool start(const std::string& sha256, size_t size);
  bool feed(const uint8_t* data, size_t size);
  void abort();

  bool active() const { return running; }
  bool complete() const { return assembler.phase() == firmware_v4::Assembler::Phase::Complete; }
  studio_v4::Error error() const { return failure != studio_v4::Error::None ? failure : assembler.error(); }
  size_t streamOffset() const { return assembler.streamOffset(); }
  size_t rawDone() const { return assembler.rawDone(); }
  static const char* path();

  // firmware_v4::Io
  bool write(const uint8_t* data, size_t size) override;
  bool stageBegin() override;
  bool stageWrite(const uint8_t* data, size_t size) override;
  bool stageRewind() override;
  int stageRead(uint8_t* data, size_t size) override;
  bool checkpoint(size_t rawDone, size_t streamOffset) override;

 private:
  FirmwareReceiver() = default;
  void closeFiles();

  firmware_v4::Assembler assembler;
  studio_v4::Error failure = studio_v4::Error::None;
  bool running = false;
  std::string sha;
  size_t size = 0;
  HalFile output, stage;
  size_t stageBytes = 0, stageReadAt = 0;
};
