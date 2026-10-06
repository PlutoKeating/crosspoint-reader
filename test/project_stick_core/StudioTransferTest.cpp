// BLE transfer protocol 4 receiver (lib/ProjectStick/StudioTransfer): the
// mini program's vectors (BleV4Vectors.h, generated from Project.StockStick
// miniprogram/tests/fixtures/ble-v4-vectors.json) drive the Assembler through
// an in-memory Io; the reconstructed file must hash to the program's SHA-256.
#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "BleCrypto.h"
#include "BleSetupProtocol.h"
#include "BleV4Vectors.h"
#include "StudioTransfer.h"

// The vendored uzlib omits its checksum sources; the device link drops
// uzlib_uncompress_chksum (unused, raw deflate has no checksum), the host
// link needs these to resolve.
extern "C" uint32_t uzlib_adler32(const void*, unsigned int, uint32_t) { return 0; }
extern "C" uint32_t uzlib_crc32(const void*, unsigned int, uint32_t) { return 0; }

using namespace studio_v4;
namespace vec = ble_v4_vectors;

namespace {
using Bytes = std::vector<uint8_t>;

Bytes unhex(const std::string& hex) {
  Bytes out;
  EXPECT_TRUE(ble_setup::unhex(hex, out)) << hex.substr(0, 16);
  return out;
}
std::string sha256Hex(const Bytes& data) {
  ble_crypto::Sha256 sha;
  sha.update(data.data(), data.size());
  uint8_t digest[32];
  sha.finish(digest);
  return ble_setup::hex(digest, 32);
}
Digest digestOf(const std::string& hexDigest) {
  Digest d{};
  EXPECT_TRUE(parseDigest(hexDigest.c_str(), d));
  return d;
}
std::string digestHex(const Digest& d) { return ble_setup::hex(d.data(), d.size()); }

// AES-256-CTR from stream offset `offset` (counter = N + offset / 16).
Bytes ctrAt(const Bytes& key, const Bytes& nonce, size_t offset, const Bytes& data) {
  uint8_t counter[16];
  memcpy(counter, nonce.data(), 16);
  uint64_t blocks = offset / 16;
  for (int i = 15; i >= 0 && blocks; --i) {
    const uint64_t sum = counter[i] + (blocks & 255);
    counter[i] = sum & 255;
    blocks = (blocks >> 8) + (sum >> 8);
  }
  const size_t skip = offset % 16;
  Bytes in(skip + data.size(), 0), out(in.size());
  memcpy(in.data() + skip, data.data(), data.size());
  EXPECT_TRUE(ble_crypto::aes256Ctr(key.data(), counter, in.data(), out.data(), in.size()));
  return Bytes(out.begin() + skip, out.end());
}

// Pulls `"sha256":"<hex>"` values in order from the vector program's header
// JSON (the device uses ArduinoJson on the card file; the test header is fixed).
std::vector<Digest> headerDigests(const Bytes& header) {
  std::vector<Digest> out;
  const std::string json(header.begin() + 8, header.end());
  const std::string key = "\"sha256\":\"";
  for (size_t at = json.find(key); at != std::string::npos; at = json.find(key, at + 1))
    out.push_back(digestOf(json.substr(at + key.size(), 64)));
  return out;
}

class MemoryIo : public Io {
 public:
  Bytes output, stage;
  size_t stageRead_ = 0, header = 0;
  std::map<std::string, Bytes> library;  // frames the "device" already holds, by digest
  Digest single{};
  bool singleMode = false;
  std::vector<Digest> digests;
  size_t copies = 0;
  bool failWrite = false;

  bool write(const uint8_t* data, size_t size) override {
    if (failWrite) return false;
    output.insert(output.end(), data, data + size);
    return true;
  }
  bool resolve(std::vector<Digest>& frames, std::vector<bool>& have, Error& error) override {
    if (singleMode) {
      digests.assign(1, single);
      frames.clear();
    } else {
      if (output.size() < header) {
        error = Error::FrameValidation;
        return false;
      }
      digests = headerDigests(Bytes(output.begin(), output.begin() + header));
      frames = digests;
    }
    have.assign(digests.size(), false);
    for (size_t i = 0; i < digests.size(); ++i) have[i] = library.count(digestHex(digests[i])) > 0;
    return true;
  }
  bool copyFrame(size_t index) override {
    const auto it = library.find(digestHex(digests.at(index)));
    if (it == library.end()) return false;
    ++copies;
    return write(it->second.data(), it->second.size());
  }
  bool stageBegin() override {
    stage.clear();
    stageRead_ = 0;
    return true;
  }
  bool stageWrite(const uint8_t* data, size_t size) override {
    stage.insert(stage.end(), data, data + size);
    return true;
  }
  bool stageRewind() override {
    stageRead_ = 0;
    return true;
  }
  int stageRead(uint8_t* data, size_t size) override {
    const size_t count = std::min(size, stage.size() - stageRead_);
    memcpy(data, stage.data() + stageRead_, count);
    stageRead_ += count;
    return static_cast<int>(count);
  }
};

// Raw frames of the vector program, inflated by the firmware's own inflater.
const std::vector<Bytes>& rawFrames() {
  static std::vector<Bytes> frames = [] {
    std::vector<Bytes> out;
    for (const auto& f : vec::FRAMES) {
      MemoryIo io;
      const Bytes blob = unhex(f.deflate);
      io.stage = blob;
      FrameInflater inflater;
      const Digest expected = digestOf(f.digest);
      EXPECT_EQ(inflater.inflate(io, FRAME_BYTES, &expected), Error::None) << f.digest;
      out.push_back(io.output);
    }
    return out;
  }();
  return frames;
}

// Feeds `plain` in uneven chunks, as BLE writes and queue slots split it.
bool feedChunked(Assembler& a, const Bytes& plain, uint32_t seed) {
  std::mt19937 random(seed);
  size_t at = 0;
  while (at < plain.size()) {
    const size_t n = std::min<size_t>(plain.size() - at, 1 + random() % 509);
    if (!a.feed(plain.data() + at, n)) return false;
    at += n;
  }
  return true;
}
}  // namespace

TEST(StudioTransfer, ProgressAndNeedEncoding) {
  for (const auto& p : vec::PROGRESS) {
    uint8_t out[PROGRESS_BYTES];
    encodeProgress(out, p.received, static_cast<State>(p.state), static_cast<Error>(p.error), p.needCount, p.seq);
    EXPECT_EQ(ble_setup::hex(out, sizeof(out)), p.hex);
  }
  EXPECT_EQ(needHex({true, true, true}), "07");
  EXPECT_EQ(needHex({true, false, true}), "05");
  EXPECT_EQ(needHex({false}), "00");
  std::vector<bool> big(12, false);
  big[0] = big[9] = big[11] = true;
  EXPECT_EQ(needHex(big), "010a");
  std::vector<bool> parsed;
  ASSERT_TRUE(parseNeedHex("010a", 12, parsed));
  EXPECT_EQ(parsed, big);
  EXPECT_FALSE(parseNeedHex("011a", 12, parsed));  // a bit past the last frame
  EXPECT_FALSE(parseNeedHex("07", 9, parsed));     // wrong length
  EXPECT_STREQ(errorName(Error::FrameMismatch), "frame_mismatch");
  EXPECT_EQ(errorFromName("insufficient_storage"), Error::InsufficientStorage);
  EXPECT_STREQ(errorName(Error::InsufficientMemory), "insufficient_memory");
  EXPECT_EQ(errorFromName("insufficient_memory"), Error::InsufficientMemory);
  EXPECT_EQ(errorFromName("unknown"), Error::None);
}

TEST(StudioTransfer, BeginProofMatchesVectors) {
  for (const auto& c : vec::CASES) {
    const std::string message =
        beginMessage(vec::DEVICE_ID, vec::N, vec::EPOCH, vec::TASK, c.hash, c.expires, c.size, c.header, vec::TIME);
    EXPECT_EQ(message, c.message) << c.name;
    EXPECT_EQ(ble_setup::mac(vec::K, message), c.proof) << c.name;
  }
  EXPECT_EQ(ble_setup::mac(vec::K, std::string("enc|") + vec::N), vec::STREAM_KEY);
}

TEST(StudioTransfer, VectorStreamsRebuildTheProgram) {
  const Bytes key = unhex(vec::STREAM_KEY), nonce = unhex(vec::N), header = unhex(vec::PROGRAM_HEADER_HEX);
  const auto& raws = rawFrames();
  for (const auto& c : vec::CASES) {
    SCOPED_TRACE(c.name);
    const Bytes plain = unhex(c.plain);
    EXPECT_EQ(ctrAt(key, nonce, 0, plain), unhex(c.encrypted));
    for (uint32_t seed : {1u, 2u, 3u}) {
      MemoryIo io;
      io.header = c.header;
      for (size_t i = 0; i < c.haveCount; ++i) {
        const Bytes& raw = c.single ? raws[1] : raws[c.have[i]];
        io.library[sha256Hex(raw)] = raw;
      }
      Assembler a;
      Digest single{};
      if (c.single) {
        io.singleMode = true;
        io.single = single = digestOf(c.hash);
      }
      // The device decrypts what it receives; the stream it was sent decrypts to `plain`.
      const Bytes decrypted = ctrAt(key, nonce, 0, unhex(c.encrypted));
      ASSERT_TRUE(a.begin(io, c.header, c.size, c.single ? &single : nullptr, 0, false));
      ASSERT_TRUE(feedChunked(a, decrypted, seed)) << errorName(a.error());
      EXPECT_EQ(a.phase(), Assembler::Phase::Complete);
      EXPECT_EQ(needHex(a.need()), c.need);
      EXPECT_EQ(a.needCount(), c.needCount);
      EXPECT_EQ(io.copies, c.haveCount);
      EXPECT_EQ(io.output.size(), c.size);
      EXPECT_EQ(sha256Hex(io.output), c.hash);
      if (!c.single) {
        EXPECT_EQ(Bytes(io.output.begin(), io.output.begin() + vec::PROGRAM_HEADER), header);
      }
    }
  }
}

TEST(StudioTransfer, VectorResumes) {
  const Bytes key = unhex(vec::STREAM_KEY), nonce = unhex(vec::N), header = unhex(vec::PROGRAM_HEADER_HEX);
  const auto& raws = rawFrames();
  for (const auto& c : vec::CASES) {
    for (size_t r = 0; r < c.resumeCount; ++r) {
      const auto& resume = c.resumes[r];
      SCOPED_TRACE(std::string(c.name) + " resume at " + std::to_string(resume.received));
      MemoryIo io;
      io.header = c.header;
      for (size_t i = 0; i < c.haveCount; ++i) {
        const Bytes& raw = c.single ? raws[1] : raws[c.have[i]];
        io.library[sha256Hex(raw)] = raw;
      }
      Digest single{};
      size_t framesDone = 0;
      if (c.single) {
        io.singleMode = true;
        io.single = single = digestOf(c.hash);
      } else {
        // What the interrupted session left in the output: the header, plus
        // the first frame when the resume point is past its record.
        io.output = header;
        if (resume.received > c.header) {
          io.output.insert(io.output.end(), raws[0].begin(), raws[0].end());
          framesDone = 1;
        }
      }
      Assembler a;
      ASSERT_TRUE(a.begin(io, c.header, c.size, c.single ? &single : nullptr, framesDone, !c.single));
      EXPECT_EQ(needHex(a.need()), resume.need);
      const Bytes decrypted = ctrAt(key, nonce, resume.received, unhex(resume.encrypted));
      ASSERT_TRUE(feedChunked(a, decrypted, 7)) << errorName(a.error());
      EXPECT_EQ(a.phase(), Assembler::Phase::Complete);
      EXPECT_EQ(sha256Hex(io.output), c.hash);
    }
  }
}

TEST(StudioTransfer, RejectsMalformedRecords) {
  const auto& raws = rawFrames();
  const Bytes header = unhex(vec::PROGRAM_HEADER_HEX);
  auto program = [&](MemoryIo& io, Assembler& a) {
    io.header = vec::PROGRAM_HEADER;
    ASSERT_TRUE(a.begin(io, vec::PROGRAM_HEADER, vec::PROGRAM_SIZE, nullptr, 0, false));
    ASSERT_TRUE(a.feed(header.data(), header.size()));
    ASSERT_EQ(a.phase(), Assembler::Phase::Frames);
  };
  auto record = [](uint16_t index, const Bytes& blob) {
    Bytes out{uint8_t(index), uint8_t(index >> 8), uint8_t(blob.size()), uint8_t(blob.size() >> 8),
              uint8_t(blob.size() >> 16), uint8_t(blob.size() >> 24)};
    out.insert(out.end(), blob.begin(), blob.end());
    return out;
  };
  const Bytes f0 = unhex(vec::FRAMES[0].deflate), f1 = unhex(vec::FRAMES[1].deflate);
  {  // out-of-order record
    MemoryIo io;
    Assembler a;
    program(io, a);
    const Bytes r = record(1, f1);
    EXPECT_FALSE(a.feed(r.data(), r.size()));
    EXPECT_EQ(a.error(), Error::FrameMismatch);
  }
  {  // record carrying another frame's pixels
    MemoryIo io;
    Assembler a;
    program(io, a);
    const Bytes r = record(0, f1);
    EXPECT_FALSE(a.feed(r.data(), r.size()));
    EXPECT_EQ(a.error(), Error::FrameMismatch);
  }
  {  // corrupted deflate data
    MemoryIo io;
    Assembler a;
    program(io, a);
    Bytes bad = f0;
    bad[bad.size() / 2] ^= 0x5a;
    const Bytes r = record(0, bad);
    EXPECT_FALSE(a.feed(r.data(), r.size()));
    EXPECT_EQ(a.error(), Error::FrameMismatch);
  }
  {  // oversized record length
    MemoryIo io;
    Assembler a;
    program(io, a);
    const uint8_t r[] = {0, 0, 0xff, 0xff, 0xff, 0x00};
    EXPECT_FALSE(a.feed(r, sizeof(r)));
    EXPECT_EQ(a.error(), Error::FrameMismatch);
  }
  {  // bytes after the last frame
    MemoryIo io;
    io.header = vec::PROGRAM_HEADER;
    for (const auto& raw : raws) io.library[sha256Hex(raw)] = raw;
    Assembler a;
    ASSERT_TRUE(a.begin(io, vec::PROGRAM_HEADER, vec::PROGRAM_SIZE, nullptr, 0, false));
    ASSERT_TRUE(a.feed(header.data(), header.size()));
    EXPECT_EQ(a.phase(), Assembler::Phase::Complete);
    const uint8_t extra = 0;
    EXPECT_FALSE(a.feed(&extra, 1));
    EXPECT_EQ(a.error(), Error::InvalidChunk);
  }
  {  // storage failure
    MemoryIo io;
    Assembler a;
    program(io, a);
    io.failWrite = true;
    const Bytes r = record(0, f0);
    EXPECT_FALSE(a.feed(r.data(), r.size()));
    EXPECT_EQ(a.error(), Error::StorageFailure);
  }
  {  // header with the wrong frame count for `size`
    MemoryIo io;
    io.header = vec::PROGRAM_HEADER;
    Assembler a;
    ASSERT_TRUE(a.begin(io, vec::PROGRAM_HEADER, vec::PROGRAM_SIZE + FRAME_BYTES, nullptr, 0, false));
    EXPECT_FALSE(a.feed(header.data(), header.size()));
    EXPECT_EQ(a.error(), Error::FrameValidation);
  }
}

// Full-size check with the real 17-frame official plan (892 KB). Run with
//   STUDIO_V4_DEMO_SSP=<demo.ssp> STUDIO_V4_DEMO_DIR=<dir with demo-*.stream>
// (streams from the independent reference generator, test/project_stick_core/ble_v4_reference.py).
TEST(StudioTransfer, DemoPlanFullSize) {
  const char* sspPath = std::getenv("STUDIO_V4_DEMO_SSP");
  const char* dir = std::getenv("STUDIO_V4_DEMO_DIR");
  if (!sspPath || !dir) GTEST_SKIP() << "STUDIO_V4_DEMO_SSP / STUDIO_V4_DEMO_DIR not set";
  auto read = [](const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), {});
  };
  const Bytes ssp = read(sspPath);
  ASSERT_GT(ssp.size(), 8u);
  const size_t header = 8 + (size_t(ssp[4]) | size_t(ssp[5]) << 8 | size_t(ssp[6]) << 16 | size_t(ssp[7]) << 24);
  const size_t frames = (ssp.size() - header) / FRAME_BYTES;
  struct Run {
    const char* stream;
    std::vector<size_t> cached;
  };
  std::vector<size_t> all16;
  for (size_t i = 0; i < frames; ++i)
    if (i != 3) all16.push_back(i);
  for (const Run& run : {Run{"demo-full.stream", {}}, Run{"demo-one.stream", all16}}) {
    SCOPED_TRACE(run.stream);
    const Bytes stream = read(std::string(dir) + "/" + run.stream);
    ASSERT_FALSE(stream.empty());
    MemoryIo io;
    io.header = header;
    for (size_t i : run.cached) {
      const Bytes raw(ssp.begin() + header + i * FRAME_BYTES, ssp.begin() + header + (i + 1) * FRAME_BYTES);
      io.library[sha256Hex(raw)] = raw;
    }
    Assembler a;
    ASSERT_TRUE(a.begin(io, header, ssp.size(), nullptr, 0, false));
    ASSERT_TRUE(feedChunked(a, stream, 11)) << errorName(a.error());
    EXPECT_EQ(a.phase(), Assembler::Phase::Complete);
    EXPECT_EQ(io.output, ssp);
    std::printf("[ demo ] %s: %zu stream bytes rebuild %zu bytes (%zu frames, %zu copied locally)\n", run.stream,
                stream.size(), ssp.size(), frames, io.copies);
  }
}

// 2.7.10: a reconnecting phone's begin4 is not refused while the previous
// session's abort is still queued on the writer.
TEST(StudioTransfer, BeginBusyRule) {
  using studio_v4::beginRefusedAsBusy;
  EXPECT_FALSE(beginRefusedAsBusy(false, false, false, 36));  // idle device
  EXPECT_TRUE(beginRefusedAsBusy(true, false, false, 36));    // a live transfer
  EXPECT_FALSE(beginRefusedAsBusy(true, true, false, 36));    // the old one is being aborted: resume
  EXPECT_TRUE(beginRefusedAsBusy(false, false, true, 36));    // a start or commit already queued
  EXPECT_TRUE(beginRefusedAsBusy(false, false, false, 35));   // malformed task id
}
