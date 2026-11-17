#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "Mp3Reader.h"
#include "ReaderTestSupport.h"

using namespace audiobook;

namespace {
struct Opened {
  MemorySource source;
  CachedReader cache;
  Mp3Reader reader;
  bool ok = false;
  explicit Opened(std::vector<uint8_t> data, size_t maxRead = SIZE_MAX) : source(std::move(data), maxRead) {
    ok = cache.init(&source, 4096) && reader.open(cache);
  }
};

struct Scan {
  size_t packets = 0;
  uint64_t lastStartMs = 0;
  bool monotonic = true;
  bool allSynced = true;
  int end = 0;
};

Scan scanAll(Mp3Reader& reader) {
  Scan s;
  uint8_t buf[MAX_PACKET_BYTES];
  uint64_t startMs = 0;
  int n = 0;
  while ((n = reader.nextPacket(buf, sizeof(buf), startMs)) > 0) {
    if (s.packets > 0 && startMs < s.lastStartMs) s.monotonic = false;
    Mp3FrameHeader h;
    if (!parseMp3Header(buf, h) || h.length != n) s.allSynced = false;
    s.lastStartMs = startMs;
    s.packets++;
  }
  s.end = n;
  return s;
}
}  // namespace

TEST(Mp3Header, ParsesMpeg1AndMpeg2LayerIII) {
  const uint8_t mpeg1[4] = {0xFF, 0xFB, 0x90, 0x00};  // 128 kbps, 44.1 kHz, stereo
  Mp3FrameHeader h;
  ASSERT_TRUE(parseMp3Header(mpeg1, h));
  EXPECT_EQ(h.sampleRate, 44100u);
  EXPECT_EQ(h.bitrate, 128000u);
  EXPECT_EQ(h.length, 417);
  EXPECT_EQ(h.samplesPerFrame, 1152);
  EXPECT_EQ(h.channels, 2);

  const uint8_t mpeg2Mono[4] = {0xFF, 0xF3, 0x40, 0xC0};  // 32 kbps, 22.05 kHz, mono
  ASSERT_TRUE(parseMp3Header(mpeg2Mono, h));
  EXPECT_EQ(h.sampleRate, 22050u);
  EXPECT_EQ(h.samplesPerFrame, 576);
  EXPECT_EQ(h.channels, 1);
  EXPECT_EQ(h.length, 72 * 32000 / 22050);
}

TEST(Mp3Header, RejectsOtherLayersAndReservedValues) {
  Mp3FrameHeader h;
  const uint8_t layer2[4] = {0xFF, 0xFD, 0x90, 0x00};
  const uint8_t freeFormat[4] = {0xFF, 0xFB, 0x00, 0x00};
  const uint8_t badRate[4] = {0xFF, 0xFB, 0x9C, 0x00};
  const uint8_t noSync[4] = {0xFF, 0x1B, 0x90, 0x00};
  EXPECT_FALSE(parseMp3Header(layer2, h));
  EXPECT_FALSE(parseMp3Header(freeFormat, h));
  EXPECT_FALSE(parseMp3Header(badRate, h));
  EXPECT_FALSE(parseMp3Header(noSync, h));
}

TEST(Mp3Reader, CbrWithTagsReadsEveryFrameAndStopsBeforeId3v1) {
  Opened f(loadFixture("tone_cbr.mp3"));
  ASSERT_TRUE(f.ok);
  EXPECT_EQ(f.reader.sampleRate(), 22050u);
  EXPECT_EQ(f.reader.channels(), 1);
  EXPECT_NEAR(static_cast<double>(f.reader.durationMs()), 2000.0, 80.0);

  const Scan s = scanAll(f.reader);
  EXPECT_EQ(s.end, 0);
  EXPECT_TRUE(s.monotonic);
  EXPECT_TRUE(s.allSynced);
  // 2 s at 576 samples per frame, plus encoder padding.
  EXPECT_GE(s.packets, 76u);
  EXPECT_LE(s.packets, 80u);
  EXPECT_NEAR(static_cast<double>(s.lastStartMs), 2000.0, 80.0);
}

TEST(Mp3Reader, ShortSourceReadsGiveTheSameFrames) {
  Opened whole(loadFixture("tone_cbr.mp3"));
  Opened chunked(loadFixture("tone_cbr.mp3"), 7);
  ASSERT_TRUE(whole.ok);
  ASSERT_TRUE(chunked.ok);
  EXPECT_EQ(scanAll(whole.reader).packets, scanAll(chunked.reader).packets);
}

TEST(Mp3Reader, VbrUsesXingFrameCountAndToc) {
  Opened f(loadFixture("tone_vbr.mp3"));
  ASSERT_TRUE(f.ok);
  EXPECT_EQ(f.reader.sampleRate(), 44100u);
  EXPECT_EQ(f.reader.channels(), 2);
  EXPECT_NEAR(static_cast<double>(f.reader.durationMs()), 3000.0, 60.0);

  uint64_t actual = 0;
  ASSERT_TRUE(f.reader.seekMs(1500, actual));
  EXPECT_NEAR(static_cast<double>(actual), 1500.0, 30.0);
  uint8_t buf[MAX_PACKET_BYTES];
  uint64_t startMs = 0;
  ASSERT_GT(f.reader.nextPacket(buf, sizeof(buf), startMs), 0);
  EXPECT_EQ(startMs, actual);

  // The fixture is silent for 1.5 s, then noise, and the silence holds only
  // about a quarter of the bytes. At 1.3 s the TOC lands in the silence; an
  // average-bitrate guess (43% of the bytes) would land in the noise.
  ASSERT_TRUE(f.reader.seekMs(1300, actual));
  const int silentFrame = f.reader.nextPacket(buf, sizeof(buf), startMs);
  ASSERT_TRUE(f.reader.seekMs(1700, actual));
  const int noisyFrame = f.reader.nextPacket(buf, sizeof(buf), startMs);
  ASSERT_GT(silentFrame, 0);
  EXPECT_GT(noisyFrame, 2 * silentFrame);
}

TEST(Mp3Reader, SeekPastEndReportsEndOfTrack) {
  Opened f(loadFixture("tone_cbr.mp3"));
  ASSERT_TRUE(f.ok);
  uint64_t actual = 0;
  ASSERT_TRUE(f.reader.seekMs(60000, actual));
  EXPECT_EQ(actual, f.reader.durationMs());
  uint8_t buf[MAX_PACKET_BYTES];
  uint64_t startMs = 0;
  EXPECT_EQ(f.reader.nextPacket(buf, sizeof(buf), startMs), 0);
}

TEST(Mp3Reader, TruncatedFileEndsCleanly) {
  auto data = loadFixture("tone_cbr.mp3");
  data.resize(data.size() / 2 + 37);  // cut inside a frame
  Opened f(std::move(data));
  ASSERT_TRUE(f.ok);
  const Scan s = scanAll(f.reader);
  EXPECT_EQ(s.end, 0);
  EXPECT_GT(s.packets, 30u);
}

// Builds an ID3v2.3 tag of `bodyBytes` zeros (larger than the sync search).
std::vector<uint8_t> id3Tag(const uint32_t bodyBytes) {
  std::vector<uint8_t> tag = {'I',
                              'D',
                              '3',
                              3,
                              0,
                              0,
                              static_cast<uint8_t>(bodyBytes >> 21 & 0x7F),
                              static_cast<uint8_t>(bodyBytes >> 14 & 0x7F),
                              static_cast<uint8_t>(bodyBytes >> 7 & 0x7F),
                              static_cast<uint8_t>(bodyBytes & 0x7F)};
  tag.resize(10 + bodyBytes, 0);
  return tag;
}

TEST(Mp3Reader, SkipsATagBetweenFramesMidFile) {
  auto data = loadFixture("tone_cbr.mp3");
  Opened plain(data);
  ASSERT_TRUE(plain.ok);
  const size_t expected = scanAll(plain.reader).packets;

  // Walk past the leading ID3v2 tag, then 41 frames (the Info frame + 40).
  ASSERT_EQ(std::memcmp(data.data(), "ID3", 3), 0);
  size_t offset = 10 + (static_cast<size_t>(data[6] & 0x7F) << 21 | static_cast<size_t>(data[7] & 0x7F) << 14 |
                        static_cast<size_t>(data[8] & 0x7F) << 7 | (data[9] & 0x7F));
  for (int i = 0; i < 41; i++) {
    Mp3FrameHeader h;
    ASSERT_TRUE(parseMp3Header(&data[offset], h));
    offset += h.length;
  }
  const auto tag = id3Tag(70000);
  data.insert(data.begin() + static_cast<std::ptrdiff_t>(offset), tag.begin(), tag.end());
  Opened tagged(std::move(data));
  ASSERT_TRUE(tagged.ok);
  const Scan s = scanAll(tagged.reader);
  EXPECT_EQ(s.end, 0);
  EXPECT_EQ(s.packets, expected);
}

TEST(Mp3Reader, TrailingApeTagEndsTheTrack) {
  auto data = loadFixture("tone_cbr.mp3");
  // Hide the Info frame count, so only the APE footer can mark the audio end.
  const auto info = std::search(data.begin(), data.end(), std::begin("Info"), std::end("Info") - 1);
  ASSERT_NE(info, data.end());
  std::copy_n("Junk", 4, info);
  Opened plain(data);
  ASSERT_TRUE(plain.ok);
  const size_t expected = scanAll(plain.reader).packets;

  // APEv2: header, 70 KB of items, footer; inserted before the ID3v1 tag.
  constexpr uint32_t items = 70000;
  const auto apeBlock = [](const uint32_t size, const bool isHeader) {
    std::vector<uint8_t> b = {'A', 'P', 'E', 'T', 'A', 'G', 'E', 'X', 0xD0, 0x07, 0, 0};
    for (int i = 0; i < 4; i++) b.push_back(static_cast<uint8_t>(size >> (8 * i)));
    for (int i = 0; i < 4; i++) b.push_back(0);  // item count
    const uint32_t flags = 0x80000000u | (isHeader ? 0x20000000u : 0);
    for (int i = 0; i < 4; i++) b.push_back(static_cast<uint8_t>(flags >> (8 * i)));
    b.resize(32, 0);
    return b;
  };
  std::vector<uint8_t> tag = apeBlock(items + 32, true);
  tag.resize(tag.size() + items, 0);
  const auto footer = apeBlock(items + 32, false);
  tag.insert(tag.end(), footer.begin(), footer.end());
  data.insert(data.end() - 128, tag.begin(), tag.end());

  Opened tagged(std::move(data));
  ASSERT_TRUE(tagged.ok);
  const Scan s = scanAll(tagged.reader);
  EXPECT_EQ(s.end, 0);
  EXPECT_EQ(s.packets, expected);
}

TEST(Mp3Reader, JunkPastTheFrameCountEndsTheTrack) {
  auto data = loadFixture("tone_cbr.mp3");
  Opened plain(data);
  ASSERT_TRUE(plain.ok);
  const size_t expected = scanAll(plain.reader).packets;

  // More unknown bytes than a resync searches, before the ID3v1 tag: with
  // the Info frame count already reached they end the track, not an error.
  data.insert(data.end() - 128, 70000, 0);
  Opened padded(std::move(data));
  ASSERT_TRUE(padded.ok);
  const Scan s = scanAll(padded.reader);
  EXPECT_EQ(s.end, 0);
  EXPECT_EQ(s.packets, expected);
}

TEST(Mp3Reader, MergedFilesPlayPastTheFirstPartsFrameCount) {
  const auto part = loadFixture("tone_cbr.mp3");
  Opened single(part);
  ASSERT_TRUE(single.ok);
  const size_t perPart = scanAll(single.reader).packets;

  // Joined byte for byte: part two's tags sit mid-file and part one's Info
  // frame count covers only part one.
  std::vector<uint8_t> merged(part.begin(), part.end());
  merged.insert(merged.end(), part.begin(), part.end());
  Opened joined(std::move(merged));
  ASSERT_TRUE(joined.ok);
  const Scan s = scanAll(joined.reader);
  EXPECT_EQ(s.end, 0);
  EXPECT_GE(s.packets, 2 * perPart);
}

TEST(Mp3Reader, LostSyncMidFileIsAnErrorNotTheEnd) {
  auto data = loadFixture("tone_cbr.mp3");
  // Overwrite 70 KB after the first frames with zeros: no tag, no sync.
  std::vector<uint8_t> big(data.begin(), data.begin() + 2000);
  big.resize(big.size() + 70000, 0);
  big.insert(big.end(), data.begin() + 2000, data.end());
  Opened f(std::move(big));
  ASSERT_TRUE(f.ok);
  EXPECT_LT(scanAll(f.reader).end, 0);
}

TEST(Mp3Reader, RejectsNonMp3Data) {
  std::vector<uint8_t> noise(20000);
  for (size_t i = 0; i < noise.size(); i++) noise[i] = static_cast<uint8_t>(i * 131 + 7);
  Opened f(std::move(noise));
  EXPECT_FALSE(f.ok);
}

TEST(Mp3Reader, SmallPacketBufferIsAnError) {
  Opened f(loadFixture("tone_vbr.mp3"));
  ASSERT_TRUE(f.ok);
  uint8_t buf[16];
  uint64_t startMs = 0;
  EXPECT_LT(f.reader.nextPacket(buf, sizeof(buf), startMs), 0);
}
