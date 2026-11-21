#pragma once

// Decoded PCM -> 16 kHz mono at the listener's speed, pitch preserved.
// The Metalio audio module clocks I2S at a fixed 16 kHz, so every source is
// downmixed, low-passed when it is resampled down, and fed to Sonic, which
// does both the time stretch (speed) and the resampling (rate).

#include <cstddef>
#include <cstdint>

struct sonicStreamStruct;

namespace audiobook {

inline constexpr uint32_t OUTPUT_SAMPLE_RATE = 16000;

inline constexpr float PLAYBACK_SPEEDS[] = {0.75f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f};
inline constexpr uint8_t PLAYBACK_SPEED_COUNT = sizeof(PLAYBACK_SPEEDS) / sizeof(PLAYBACK_SPEEDS[0]);
inline constexpr uint8_t DEFAULT_SPEED_INDEX = 1;

// Out-of-range indexes fall back to normal speed.
float playbackSpeed(uint8_t index);
// The speed `delta` steps from `index`, held at the slowest and fastest
// (no wrap-around). An out-of-range index steps from normal speed.
uint8_t steppedSpeedIndex(uint8_t index, int delta);

class PcmPipeline {
 public:
  PcmPipeline() = default;
  ~PcmPipeline();
  PcmPipeline(const PcmPipeline&) = delete;
  PcmPipeline& operator=(const PcmPipeline&) = delete;

  // Prepares for `sourceRate` input. Keeps the existing stream when the rate
  // is unchanged. Returns false when Sonic cannot allocate its buffers.
  bool configure(uint32_t sourceRate, float speed);
  void setSpeed(float speed);
  // Drops queued audio and filter history, e.g. after a seek.
  void reset();

  // Downmixes `frames` interleaved frames in place (the first `frames`
  // samples of `interleaved` become mono) and queues them.
  bool write(int16_t* interleaved, size_t frames, uint8_t channels);
  // Processes the queued tail at end of stream.
  bool finish();

  size_t available() const;
  // Copies up to `capacity` 16 kHz mono samples. Returns the count.
  size_t read(int16_t* out, size_t capacity);

  uint32_t sourceRate() const { return sourceRate_; }

 private:
  struct Biquad {
    float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;
  };

  void destroy();
  void lowPass(int16_t* samples, size_t count);

  sonicStreamStruct* stream_ = nullptr;
  uint32_t sourceRate_ = 0;
  bool filtering_ = false;
  Biquad sections_[2];
};

}  // namespace audiobook
