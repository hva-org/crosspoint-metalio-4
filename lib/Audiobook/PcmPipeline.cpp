#include "PcmPipeline.h"

#include <sonic.h>

#include <cmath>

namespace audiobook {
namespace {
// Sonic's 12-tap resampler filters at the input Nyquist, so content between
// 8 kHz and the source Nyquist would alias when resampling down to 16 kHz.
// A 4th-order Butterworth at 6.5 kHz removes about 21 dB at 12 kHz.
constexpr float LOW_PASS_HZ = 6500.0f;
constexpr float BUTTERWORTH_Q[2] = {0.54119610f, 1.30656296f};
constexpr float PI = 3.14159265358979f;

int16_t clip16(const float v) {
  if (v >= 32767.0f) return 32767;
  if (v <= -32768.0f) return -32768;
  return static_cast<int16_t>(std::lrintf(v));
}
}  // namespace

float playbackSpeed(const uint8_t index) {
  return index < PLAYBACK_SPEED_COUNT ? PLAYBACK_SPEEDS[index] : PLAYBACK_SPEEDS[DEFAULT_SPEED_INDEX];
}

uint8_t steppedSpeedIndex(const uint8_t index, const int delta) {
  const int from = index < PLAYBACK_SPEED_COUNT ? index : DEFAULT_SPEED_INDEX;
  const int next = from + delta;
  if (next < 0) return 0;
  if (next >= PLAYBACK_SPEED_COUNT) return PLAYBACK_SPEED_COUNT - 1;
  return static_cast<uint8_t>(next);
}

PcmPipeline::~PcmPipeline() { destroy(); }

void PcmPipeline::destroy() {
  if (stream_) sonicDestroyStream(stream_);
  stream_ = nullptr;
  sourceRate_ = 0;
}

bool PcmPipeline::configure(const uint32_t sourceRate, const float speed) {
  if (sourceRate == 0) return false;
  if (!stream_ || sourceRate != sourceRate_) {
    destroy();
    stream_ = sonicCreateStream(static_cast<int>(sourceRate), 1);
    if (!stream_) return false;
    sourceRate_ = sourceRate;
    sonicSetRate(stream_, static_cast<float>(sourceRate) / OUTPUT_SAMPLE_RATE);
  }
  setSpeed(speed);

  filtering_ = sourceRate > OUTPUT_SAMPLE_RATE;
  if (filtering_) {
    const float w0 = 2.0f * PI * LOW_PASS_HZ / static_cast<float>(sourceRate);
    const float cosW0 = std::cos(w0);
    for (int i = 0; i < 2; i++) {
      const float alpha = std::sin(w0) / (2.0f * BUTTERWORTH_Q[i]);
      const float a0 = 1.0f + alpha;
      Biquad& s = sections_[i];
      s.b0 = (1.0f - cosW0) / 2.0f / a0;
      s.b1 = (1.0f - cosW0) / a0;
      s.b2 = s.b0;
      s.a1 = -2.0f * cosW0 / a0;
      s.a2 = (1.0f - alpha) / a0;
    }
  }
  reset();
  return true;
}

void PcmPipeline::setSpeed(const float speed) {
  if (stream_) sonicSetSpeed(stream_, speed);
}

void PcmPipeline::reset() {
  for (auto& s : sections_) s.z1 = s.z2 = 0;
  if (!stream_) return;
  // Sonic has no reset call. Flushing and draining empties it without
  // reallocating its buffers.
  sonicFlushStream(stream_);
  int16_t discard[128];
  while (sonicReadShortFromStream(stream_, discard, static_cast<int>(sizeof(discard) / sizeof(discard[0]))) > 0) {
  }
}

void PcmPipeline::lowPass(int16_t* samples, const size_t count) {
  for (size_t i = 0; i < count; i++) {
    float x = samples[i];
    for (auto& s : sections_) {
      // Transposed direct form II.
      const float y = s.b0 * x + s.z1;
      s.z1 = s.b1 * x - s.a1 * y + s.z2;
      s.z2 = s.b2 * x - s.a2 * y;
      x = y;
    }
    samples[i] = clip16(x);
  }
}

bool PcmPipeline::write(int16_t* interleaved, const size_t frames, const uint8_t channels) {
  if (!stream_ || channels == 0) return false;
  if (frames == 0) return true;
  if (channels > 1) {
    for (size_t i = 0; i < frames; i++) {
      const int32_t left = interleaved[i * channels];
      const int32_t right = interleaved[i * channels + 1];
      interleaved[i] = static_cast<int16_t>((left + right) / 2);
    }
  }
  if (filtering_) lowPass(interleaved, frames);
  return sonicWriteShortToStream(stream_, interleaved, static_cast<int>(frames)) != 0;
}

bool PcmPipeline::finish() { return stream_ && sonicFlushStream(stream_) != 0; }

size_t PcmPipeline::available() const { return stream_ ? static_cast<size_t>(sonicSamplesAvailable(stream_)) : 0; }

size_t PcmPipeline::read(int16_t* out, const size_t capacity) {
  if (!stream_ || capacity == 0) return 0;
  const int n = sonicReadShortFromStream(stream_, out, static_cast<int>(capacity));
  return n > 0 ? static_cast<size_t>(n) : 0;
}

}  // namespace audiobook
