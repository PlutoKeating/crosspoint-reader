#pragma once
#include <HalStorage.h>
#include <StudioTransfer.h>

#include <cstdint>
#include <string>
#include <vector>

// Device side of BLE transfer protocol 4 (Project.StockStick
// docs/product/BLE-TRANSFER-V4.md). Runs on the BLE writer task only: it
// rebuilds the full SSP1 file (or single frame) in StudioFrame's incoming.bin
// from the decrypted stream, copying frames the device already holds from the
// kept program files and inflating the rest from deflate records staged on the
// card. StudioFrame then verifies the SHA-256 and installs it as before.
//
// Frame library: the kept program files themselves (active, saved program,
// last visual). A frame is "held" when one of them contains its digest; no
// separate per-frame files, so a frame is never written twice and garbage
// collection stays StudioFrame's (it already keeps exactly those files).
class StudioReceiver final : public studio_v4::Io {
 public:
  static StudioReceiver& instance();

  struct Resume {
    size_t offset = 0;      // stream offset the phone continues from
    size_t framesDone = 0;  // frames already in incoming.bin
    bool headerDone = false;
  };
  // Resume point for a partial of `bytes` bytes of this transfer (pure: the
  // BLE callback answers begin4 with it without touching the card).
  static Resume resumeFor(size_t header, size_t size, size_t partialBytes);

  // Opens incoming.bin (resuming per `resume`) and starts the assembler.
  // On a resume or a single frame this already resolves `need`.
  bool start(const std::string& task, const std::string& hash, int64_t expires, size_t size, size_t header,
             const Resume& resume);
  bool feed(const uint8_t* data, size_t size);
  // All frames written: hands the file to StudioFrame::commit().
  bool commit();
  void abort(bool discard);

  bool active() const { return running; }
  studio_v4::Assembler::Phase phase() const { return assembler.phase(); }
  studio_v4::Error error() const { return failure != studio_v4::Error::None ? failure : assembler.error(); }
  std::string needHex() const { return studio_v4::needHex(assembler.need()); }
  size_t needCount() const { return assembler.needCount(); }
  // Bytes of the rebuilt file written so far (device progress bar).
  size_t rebuiltBytes() const;

  // studio_v4::Io
  bool write(const uint8_t* data, size_t size) override;
  bool resolve(std::vector<studio_v4::Digest>& frames, std::vector<bool>& have, studio_v4::Error& error) override;
  bool copyFrame(size_t index) override;
  bool stageBegin() override;
  bool stageWrite(const uint8_t* data, size_t size) override;
  bool stageRewind() override;
  int stageRead(uint8_t* data, size_t size) override;

 private:
  StudioReceiver() = default;
  bool readDigests(const std::string& path, size_t fileSize, std::vector<studio_v4::Digest>& out,
                   size_t& headerBytes, studio_v4::Error& error);
  void closeStage();

  studio_v4::Assembler assembler;
  studio_v4::Error failure = studio_v4::Error::None;
  bool running = false;
  std::string hash;
  size_t header = 0, size = 0;
  // Where each held frame of the incoming program lives.
  struct Source {
    uint8_t file = 0;
    uint32_t offset = 0;
  };
  std::vector<std::string> sourcePaths;
  std::vector<Source> sources;
  HalFile stage;
  size_t stageBytes = 0, stageReadAt = 0;
};
