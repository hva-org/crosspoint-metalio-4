#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "Mp4Reader.h"
#include "ReaderTestSupport.h"

using namespace audiobook;

namespace {
struct Opened {
  MemorySource source;
  CachedReader cache;
  Mp4Reader reader;
  bool ok = false;
  explicit Opened(std::vector<uint8_t> data) : source(std::move(data)) {
    ok = cache.init(&source, 4096) && reader.open(source, cache);
  }
};

size_t countPackets(Mp4Reader& reader, uint64_t* lastStartMs = nullptr, int* end = nullptr) {
  uint8_t buf[MAX_PACKET_BYTES];
  uint64_t startMs = 0, previous = 0;
  size_t count = 0;
  int n = 0;
  while ((n = reader.nextPacket(buf, sizeof(buf), startMs)) > 0) {
    EXPECT_GE(startMs, previous);
    previous = startMs;
    count++;
  }
  if (lastStartMs) *lastStartMs = previous;
  if (end) *end = n;
  return count;
}

void expectThreeChapters(const Mp4Reader& reader) {
  ASSERT_EQ(reader.chapterCount(), 3u);
  const Chapter* c = reader.chapters();
  EXPECT_EQ(c[0].startMs, 0u);
  EXPECT_NEAR(static_cast<double>(c[1].startMs), 1000.0, 1.0);
  EXPECT_NEAR(static_cast<double>(c[2].startMs), 2000.0, 1.0);
  EXPECT_STREQ(c[0].title, "Opening");
  EXPECT_STREQ(c[1].title, "Middle Part");
  EXPECT_STREQ(c[2].title, "The End");
}
}  // namespace

TEST(AacConfig, ParsesLcAndExplicitHeAac) {
  AacConfig c;
  const uint8_t lc22050Mono[2] = {0x13, 0x88};  // AOT 2, index 7, 1 channel
  ASSERT_TRUE(parseAacConfig(lc22050Mono, sizeof(lc22050Mono), c));
  EXPECT_EQ(c.objectType, 2);
  EXPECT_EQ(c.coreRate, 22050u);
  EXPECT_EQ(c.channels, 1);

  // AOT 5, core 22.05 kHz stereo, extension 44.1 kHz, core AOT 2.
  const uint8_t heAac[4] = {0x2B, 0x92, 0x08, 0x00};
  ASSERT_TRUE(parseAacConfig(heAac, sizeof(heAac), c));
  EXPECT_EQ(c.objectType, 5);
  EXPECT_EQ(c.coreRate, 22050u);
  EXPECT_EQ(c.channels, 2);
}

TEST(AacConfig, KeepsTheRawConfigForTheDecoder) {
  AacConfig c;
  // LC with backward-compatible SBR signalling (sync extension 0x2B7).
  const uint8_t lcWithSbr[5] = {0x13, 0x10, 0x56, 0xE5, 0x98};
  ASSERT_TRUE(parseAacConfig(lcWithSbr, sizeof(lcWithSbr), c));
  ASSERT_EQ(c.length, sizeof(lcWithSbr));
  EXPECT_EQ(memcmp(c.bytes, lcWithSbr, sizeof(lcWithSbr)), 0);

  uint8_t tooLong[AacConfig::MAX_BYTES + 1] = {0x13, 0x88};
  EXPECT_FALSE(parseAacConfig(tooLong, sizeof(tooLong), c));
}

TEST(AacConfig, RejectsUnsupportedObjectTypes) {
  AacConfig c;
  const uint8_t main[2] = {0x0B, 0x88};      // AOT 1 (Main)
  const uint8_t badIndex[2] = {0x16, 0x88};  // sampling index 13
  EXPECT_FALSE(parseAacConfig(main, sizeof(main), c));
  EXPECT_FALSE(parseAacConfig(badIndex, sizeof(badIndex), c));
  EXPECT_FALSE(parseAacConfig(main, 0, c));
}

TEST(Mp4Reader, FaststartAacTrack) {
  Opened f(loadFixture("tone.m4a"));
  ASSERT_TRUE(f.ok);
  EXPECT_EQ(f.reader.codec(), Codec::Aac);
  EXPECT_EQ(f.reader.aacConfig().objectType, 2);
  EXPECT_EQ(f.reader.sampleRate(), 22050u);
  EXPECT_EQ(f.reader.channels(), 1);
  EXPECT_NEAR(static_cast<double>(f.reader.durationMs()), 2000.0, 60.0);
  EXPECT_EQ(f.reader.chapterCount(), 0u);

  uint64_t last = 0;
  int end = -1;
  const size_t packets = countPackets(f.reader, &last, &end);
  EXPECT_EQ(end, 0);
  // 2 s of 1024-sample frames at 22.05 kHz, plus encoder priming.
  EXPECT_GE(packets, 43u);
  EXPECT_LE(packets, 46u);
  EXPECT_NEAR(static_cast<double>(last), 2000.0, 60.0);
}

TEST(Mp4Reader, MoovAfterMdatWithNeroChapters) {
  Opened f(loadFixture("chapters.m4b"));
  ASSERT_TRUE(f.ok);
  EXPECT_NEAR(static_cast<double>(f.reader.durationMs()), 3000.0, 60.0);
  expectThreeChapters(f.reader);
}

TEST(Mp4Reader, QuickTimeChapterTrackWhenChplIsMissing) {
  auto data = loadFixture("chapters.m4b");
  ASSERT_TRUE(renameBox(data, "chpl", "free"));
  Opened f(std::move(data));
  ASSERT_TRUE(f.ok);
  expectThreeChapters(f.reader);
}

TEST(Mp4Reader, SeekLandsOnSampleBoundary) {
  Opened f(loadFixture("chapters.m4b"));
  ASSERT_TRUE(f.ok);
  uint64_t actual = 0;
  ASSERT_TRUE(f.reader.seekMs(1500, actual));
  EXPECT_LE(actual, 1500u);
  EXPECT_GT(actual, 1400u);  // one AAC frame is 46 ms at 22.05 kHz
  uint8_t buf[MAX_PACKET_BYTES];
  uint64_t startMs = 0;
  ASSERT_GT(f.reader.nextPacket(buf, sizeof(buf), startMs), 0);
  EXPECT_EQ(startMs, actual);

  ASSERT_TRUE(f.reader.seekMs(0, actual));
  EXPECT_EQ(actual, 0u);
  ASSERT_TRUE(f.reader.seekMs(99999, actual));
  EXPECT_EQ(f.reader.nextPacket(buf, sizeof(buf), startMs), 0);
}

TEST(Mp4Reader, SeekThenReadMatchesSequentialRead) {
  Opened sequential(loadFixture("chapters.m4b"));
  Opened seeked(loadFixture("chapters.m4b"));
  ASSERT_TRUE(sequential.ok && seeked.ok);
  uint8_t a[MAX_PACKET_BYTES], b[MAX_PACKET_BYTES];
  uint64_t ta = 0, tb = 0, actual = 0;
  ASSERT_TRUE(seeked.reader.seekMs(1000, actual));
  int na = 0;
  do {
    na = sequential.reader.nextPacket(a, sizeof(a), ta);
  } while (na > 0 && ta < actual);
  const int nb = seeked.reader.nextPacket(b, sizeof(b), tb);
  ASSERT_GT(na, 0);
  ASSERT_EQ(na, nb);
  EXPECT_EQ(ta, tb);
  EXPECT_EQ(std::memcmp(a, b, static_cast<size_t>(na)), 0);
}

TEST(Mp4Reader, TruncatedMoovFailsToOpen) {
  auto data = loadFixture("chapters.m4b");
  data.resize(data.size() - 900);  // cuts into the moov box at the end
  Opened f(std::move(data));
  EXPECT_FALSE(f.ok);
}

TEST(Mp4Reader, CorruptSampleSizesStopWithoutCrashing) {
  auto data = loadFixture("tone.m4a");
  // Overwrite the stsz entries with huge sizes.
  for (size_t i = 0; i + 4 <= data.size(); i++) {
    if (std::memcmp(data.data() + i, "stsz", 4) == 0) {
      for (size_t j = i + 16; j < i + 16 + 40 && j < data.size(); j++) data[j] = 0x7F;
      break;
    }
  }
  Opened f(std::move(data));
  ASSERT_TRUE(f.ok);
  int end = 0;
  countPackets(f.reader, nullptr, &end);
  EXPECT_LT(end, 0);
}

TEST(Mp4Reader, RejectsNonMp4Data) {
  std::vector<uint8_t> noise(4096, 0x41);
  Opened f(std::move(noise));
  EXPECT_FALSE(f.ok);
}
