#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "PcmPipeline.h"

using audiobook::OUTPUT_SAMPLE_RATE;
using audiobook::PcmPipeline;

namespace {
constexpr double PI = 3.14159265358979;

std::vector<int16_t> sine(const double hz, const uint32_t rate, const double seconds, const double amplitude,
                          const uint8_t channels = 1, const bool invertRight = false) {
  const size_t frames = static_cast<size_t>(rate * seconds);
  std::vector<int16_t> out(frames * channels);
  for (size_t i = 0; i < frames; i++) {
    const auto v = static_cast<int16_t>(std::lrint(amplitude * std::sin(2 * PI * hz * i / rate)));
    out[i * channels] = v;
    if (channels == 2) out[i * channels + 1] = invertRight ? static_cast<int16_t>(-v) : v;
  }
  return out;
}

// Runs `input` through a pipeline in decoder-sized chunks and returns all output.
std::vector<int16_t> run(PcmPipeline& pipeline, std::vector<int16_t> input, const uint8_t channels) {
  constexpr size_t CHUNK = 1152;
  std::vector<int16_t> out;
  int16_t buf[512];
  const size_t frames = input.size() / channels;
  for (size_t f = 0; f < frames; f += CHUNK) {
    const size_t n = std::min(CHUNK, frames - f);
    EXPECT_TRUE(pipeline.write(input.data() + f * channels, n, channels));
    while (size_t got = pipeline.read(buf, 512)) out.insert(out.end(), buf, buf + got);
  }
  EXPECT_TRUE(pipeline.finish());
  while (size_t got = pipeline.read(buf, 512)) out.insert(out.end(), buf, buf + got);
  return out;
}

double rms(const std::vector<int16_t>& s, const size_t skip = 0) {
  double sum = 0;
  for (size_t i = skip; i < s.size(); i++) sum += static_cast<double>(s[i]) * s[i];
  return std::sqrt(sum / static_cast<double>(s.size() - skip));
}

// Estimates the dominant frequency from upward zero crossings.
double frequency(const std::vector<int16_t>& s, const uint32_t rate) {
  size_t first = 0, last = 0, crossings = 0;
  for (size_t i = 1; i < s.size(); i++) {
    if (s[i - 1] < 0 && s[i] >= 0) {
      if (crossings == 0) first = i;
      last = i;
      crossings++;
    }
  }
  if (crossings < 2) return 0;
  return static_cast<double>(crossings - 1) * rate / static_cast<double>(last - first);
}
}  // namespace

TEST(PcmPipelineSpeed, SpeedStepsStopAtTheEnds) {
  using audiobook::DEFAULT_SPEED_INDEX;
  using audiobook::PLAYBACK_SPEED_COUNT;
  using audiobook::steppedSpeedIndex;
  EXPECT_EQ(steppedSpeedIndex(1, 1), 2);
  EXPECT_EQ(steppedSpeedIndex(2, -1), 1);
  // No wrap-around at either end.
  EXPECT_EQ(steppedSpeedIndex(0, -1), 0);
  EXPECT_EQ(steppedSpeedIndex(PLAYBACK_SPEED_COUNT - 1, 1), PLAYBACK_SPEED_COUNT - 1);
  EXPECT_EQ(steppedSpeedIndex(0, 100), PLAYBACK_SPEED_COUNT - 1);
  // A bad saved index steps from normal speed.
  EXPECT_EQ(steppedSpeedIndex(200, 0), DEFAULT_SPEED_INDEX);
  EXPECT_EQ(steppedSpeedIndex(200, 1), DEFAULT_SPEED_INDEX + 1);
}

TEST(PcmPipelineSpeed, OutputLengthScalesWithSpeed) {
  for (const float speed : audiobook::PLAYBACK_SPEEDS) {
    PcmPipeline pipeline;
    ASSERT_TRUE(pipeline.configure(OUTPUT_SAMPLE_RATE, speed));
    const auto out = run(pipeline, sine(220, OUTPUT_SAMPLE_RATE, 10.0, 8000), 1);
    const double expected = 10.0 * OUTPUT_SAMPLE_RATE / speed;
    EXPECT_NEAR(static_cast<double>(out.size()), expected, expected * 0.02) << "speed " << speed;
  }
}

TEST(PcmPipelineSpeed, PitchIsPreserved) {
  PcmPipeline pipeline;
  ASSERT_TRUE(pipeline.configure(OUTPUT_SAMPLE_RATE, 1.5f));
  const auto out = run(pipeline, sine(300, OUTPUT_SAMPLE_RATE, 4.0, 8000), 1);
  EXPECT_NEAR(frequency(out, OUTPUT_SAMPLE_RATE), 300.0, 300.0 * 0.03);
}

TEST(PcmPipelineResample, ConvertsSourceRateTo16k) {
  PcmPipeline pipeline;
  ASSERT_TRUE(pipeline.configure(44100, 1.0f));
  const auto out = run(pipeline, sine(1000, 44100, 2.0, 8000), 1);
  EXPECT_NEAR(static_cast<double>(out.size()), 2.0 * OUTPUT_SAMPLE_RATE, 2.0 * OUTPUT_SAMPLE_RATE * 0.01);
  EXPECT_NEAR(frequency(out, OUTPUT_SAMPLE_RATE), 1000.0, 10.0);
}

TEST(PcmPipelineResample, AttenuatesContentThatWouldAlias) {
  // 12 kHz cannot be represented at 16 kHz; without filtering it folds to 4 kHz.
  PcmPipeline passPipeline;
  ASSERT_TRUE(passPipeline.configure(44100, 1.0f));
  const double passRms = rms(run(passPipeline, sine(1000, 44100, 2.0, 8000), 1), 1600);

  PcmPipeline stopPipeline;
  ASSERT_TRUE(stopPipeline.configure(44100, 1.0f));
  const double stopRms = rms(run(stopPipeline, sine(12000, 44100, 2.0, 8000), 1), 1600);

  EXPECT_GT(passRms, 8000 / std::sqrt(2.0) * 0.89);  // passband within 1 dB
  EXPECT_LT(stopRms, passRms * 0.1);                 // at least 20 dB down
}

TEST(PcmPipelineDownmix, StereoAveragesChannels) {
  PcmPipeline pipeline;
  ASSERT_TRUE(pipeline.configure(OUTPUT_SAMPLE_RATE, 1.0f));
  const auto out = run(pipeline, sine(440, OUTPUT_SAMPLE_RATE, 1.0, 8000, 2, /*invertRight=*/true), 2);
  EXPECT_LT(rms(out), 2.0);
}

TEST(PcmPipelineReset, DropsQueuedAudio) {
  PcmPipeline pipeline;
  ASSERT_TRUE(pipeline.configure(44100, 1.25f));
  auto input = sine(440, 44100, 0.5, 8000);
  ASSERT_TRUE(pipeline.write(input.data(), input.size(), 1));
  pipeline.reset();
  EXPECT_EQ(pipeline.available(), 0u);
}

TEST(PcmPipelineConfig, SpeedIndexFallsBackToNormal) {
  EXPECT_FLOAT_EQ(audiobook::playbackSpeed(0), 0.75f);
  EXPECT_FLOAT_EQ(audiobook::playbackSpeed(5), 2.0f);
  EXPECT_FLOAT_EQ(audiobook::playbackSpeed(6), 1.0f);
  PcmPipeline pipeline;
  EXPECT_FALSE(pipeline.configure(0, 1.0f));
  int16_t sample = 0;
  EXPECT_FALSE(pipeline.write(&sample, 1, 1));
}
