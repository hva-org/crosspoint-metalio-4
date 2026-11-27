#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "AudioDecoder.h"
#include "Mp3Reader.h"
#include "Mp4Reader.h"
#include "PcmPipeline.h"
#include "ReaderTestSupport.h"

using namespace audiobook;

namespace {
struct Decoded {
  std::vector<int16_t> pcm;  // 16 kHz mono
  PcmFormat format;
  size_t damaged = 0;
};

AacConfig parsedConfig(const std::vector<uint8_t>& bytes) {
  AacConfig config;
  EXPECT_TRUE(parseAacConfig(bytes.data(), bytes.size(), config));
  return config;
}

// Runs a whole file through reader -> decoder -> pipeline, as the device does.
// `config` replaces the file's AudioSpecificConfig when it is not empty.
Decoded decodeFile(const std::string& name, const float speed, const uint64_t seekMs = 0,
                   const std::vector<uint8_t>& config = {}) {
  Decoded out;
  MemorySource source(loadFixture(name));
  CachedReader cache;
  EXPECT_TRUE(cache.init(&source, 4096));
  Mp3Reader mp3;
  Mp4Reader mp4;
  const bool isMp3 = name.ends_with(".mp3");
  TrackReader* reader = isMp3 ? static_cast<TrackReader*>(&mp3) : &mp4;
  const bool opened = isMp3 ? mp3.open(cache) : mp4.open(source, cache);
  EXPECT_TRUE(opened);
  if (!opened) return out;

  AudioDecoder decoder;
  const AacConfig aac = isMp3 ? AacConfig{} : config.empty() ? mp4.aacConfig() : parsedConfig(config);
  EXPECT_TRUE(decoder.begin(reader->codec(), aac));
  if (seekMs > 0) {
    uint64_t actual = 0;
    EXPECT_TRUE(reader->seekMs(seekMs, actual));
    decoder.flush();
  }

  PcmPipeline pipeline;
  auto packet = std::make_unique<uint8_t[]>(MAX_PACKET_BYTES);
  auto pcm = std::make_unique<int16_t[]>(MAX_DECODED_SAMPLES);
  int16_t chunk[512];
  uint64_t startMs = 0;
  int n = 0;
  while ((n = reader->nextPacket(packet.get(), MAX_PACKET_BYTES, startMs)) > 0) {
    PcmFormat format;
    const int frames = decoder.decode(packet.get(), static_cast<size_t>(n), pcm.get(), MAX_DECODED_SAMPLES, format);
    if (frames < 0) {
      out.damaged++;
      continue;
    }
    if (frames == 0) continue;
    if (pipeline.sourceRate() != format.sampleRate) EXPECT_TRUE(pipeline.configure(format.sampleRate, speed));
    out.format = format;
    EXPECT_TRUE(pipeline.write(pcm.get(), static_cast<size_t>(frames), format.channels));
    while (size_t got = pipeline.read(chunk, 512)) out.pcm.insert(out.pcm.end(), chunk, chunk + got);
  }
  EXPECT_EQ(n, 0);
  pipeline.finish();
  while (size_t got = pipeline.read(chunk, 512)) out.pcm.insert(out.pcm.end(), chunk, chunk + got);
  return out;
}

// Measures the middle half only: encoder priming/padding and the flushed
// tail add stray zero crossings at the ends.
double frequency(const std::vector<int16_t>& s) {
  size_t first = 0, last = 0, crossings = 0;
  for (size_t i = s.size() / 4 + 1; i < s.size() * 3 / 4; i++) {
    if (s[i - 1] < 0 && s[i] >= 0) {
      if (crossings == 0) first = i;
      last = i;
      crossings++;
    }
  }
  return crossings < 2 ? 0
                       : static_cast<double>(crossings - 1) * OUTPUT_SAMPLE_RATE / static_cast<double>(last - first);
}

double rms(const std::vector<int16_t>& s, const size_t skip) {
  double sum = 0;
  for (size_t i = skip; i < s.size(); i++) sum += static_cast<double>(s[i]) * s[i];
  return s.size() > skip ? std::sqrt(sum / static_cast<double>(s.size() - skip)) : 0;
}
}  // namespace

TEST(DecodeChain, Mp3ToneComesOutAt440HzAnd16k) {
  const Decoded d = decodeFile("tone_cbr.mp3", 1.0f);
  EXPECT_EQ(d.format.sampleRate, 22050u);
  EXPECT_EQ(d.damaged, 0u);
  EXPECT_NEAR(static_cast<double>(d.pcm.size()), 2.0 * OUTPUT_SAMPLE_RATE, 0.08 * OUTPUT_SAMPLE_RATE);
  EXPECT_NEAR(frequency(d.pcm), 440.0, 5.0);
  EXPECT_GT(rms(d.pcm, 1600), 1000.0);
}

TEST(DecodeChain, AacToneComesOutAt440HzAnd16k) {
  const Decoded d = decodeFile("tone.m4a", 1.0f);
  EXPECT_EQ(d.format.sampleRate, 22050u);
  EXPECT_EQ(d.format.channels, 1);
  EXPECT_EQ(d.damaged, 0u);
  EXPECT_NEAR(static_cast<double>(d.pcm.size()), 2.0 * OUTPUT_SAMPLE_RATE, 0.1 * OUTPUT_SAMPLE_RATE);
  EXPECT_NEAR(frequency(d.pcm), 440.0, 5.0);
  EXPECT_GT(rms(d.pcm, 1600), 1000.0);
}

TEST(DecodeChain, DoubleSpeedHalvesDurationAndKeepsPitch) {
  const Decoded d = decodeFile("chapters.m4b", 2.0f);
  EXPECT_NEAR(static_cast<double>(d.pcm.size()), 1.5 * OUTPUT_SAMPLE_RATE, 0.1 * OUTPUT_SAMPLE_RATE);
  EXPECT_NEAR(frequency(d.pcm), 440.0, 10.0);
}

TEST(DecodeChain, StereoVbrMp3DecodesAfterSeek) {
  const Decoded d = decodeFile("tone_vbr.mp3", 1.0f, 1700);
  EXPECT_EQ(d.format.sampleRate, 44100u);
  EXPECT_EQ(d.format.channels, 2);
  EXPECT_EQ(d.damaged, 0u);
  // About 1.3 s of noise remains after the seek.
  EXPECT_NEAR(static_cast<double>(d.pcm.size()), 1.3 * OUTPUT_SAMPLE_RATE, 0.15 * OUTPUT_SAMPLE_RATE);
  EXPECT_GT(rms(d.pcm, 1600), 1000.0);
}

TEST(DecodeChain, AacDecodesAfterSeek) {
  const Decoded d = decodeFile("chapters.m4b", 1.0f, 1500);
  EXPECT_EQ(d.damaged, 0u);
  EXPECT_NEAR(static_cast<double>(d.pcm.size()), 1.5 * OUTPUT_SAMPLE_RATE, 0.1 * OUTPUT_SAMPLE_RATE);
  EXPECT_NEAR(frequency(d.pcm), 440.0, 5.0);
}

// The fixture has no SBR data, so these check the 2x output path (QMF banks,
// 2048-frame output) rather than SBR synthesis itself.
TEST(DecodeChain, ExplicitHeAacConfigOutputsTwiceTheCoreRate) {
  // AOT 5, core 22.05 kHz mono, extension 44.1 kHz, core AOT 2.
  const Decoded d = decodeFile("tone.m4a", 1.0f, 0, {0x2B, 0x8A, 0x08, 0x00});
  EXPECT_EQ(d.format.sampleRate, 44100u);
  EXPECT_EQ(d.format.channels, 1);
  EXPECT_EQ(d.damaged, 0u);
  EXPECT_NEAR(frequency(d.pcm), 440.0, 5.0);
}

TEST(DecodeChain, HeAacV2ConfigPlaysAsSbrMono) {
  // Same as above with AOT 29 (SBR + parametric stereo).
  const Decoded d = decodeFile("tone.m4a", 1.0f, 0, {0xEB, 0x8A, 0x08, 0x00});
  EXPECT_EQ(d.format.sampleRate, 44100u);
  EXPECT_EQ(d.format.channels, 1);
  EXPECT_EQ(d.damaged, 0u);
  EXPECT_NEAR(frequency(d.pcm), 440.0, 5.0);
}

TEST(DecodeChain, DamagedPacketsStayWithinTheOutputBuffer) {
  // Raw AAC blocks have no sync word or checksum, so junk may decode as noise.
  // What matters is that it never writes past one frame or crashes.
  std::vector<uint8_t> junk(200);
  for (size_t i = 0; i < junk.size(); i++) junk[i] = static_cast<uint8_t>(i * 97 + 13);
  auto pcm = std::make_unique<int16_t[]>(MAX_DECODED_SAMPLES);
  PcmFormat format;
  for (const Codec codec : {Codec::Aac, Codec::Mp3}) {
    AudioDecoder decoder;
    ASSERT_TRUE(decoder.begin(codec, parsedConfig({0x13, 0x88})));  // LC, 22.05 kHz mono
    for (int i = 0; i < 20; i++) {
      const int frames = decoder.decode(junk.data(), junk.size(), pcm.get(), MAX_DECODED_SAMPLES, format);
      EXPECT_LE(frames, 2048);
      if (frames > 0) EXPECT_LE(static_cast<size_t>(frames) * format.channels, MAX_DECODED_SAMPLES);
    }
    EXPECT_LT(decoder.decode(junk.data(), junk.size(), pcm.get(), 16, format), 0);
  }
}
