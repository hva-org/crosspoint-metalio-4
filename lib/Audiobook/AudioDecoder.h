#pragma once

#include <cstddef>
#include <cstdint>

#include "Mp4Reader.h"
#include "TrackReader.h"

namespace audiobook {

// One HE-AAC frame: 2048 samples per channel, two channels.
inline constexpr size_t MAX_DECODED_SAMPLES = 2048 * 2;

struct PcmFormat {
  uint32_t sampleRate = 0;
  uint8_t channels = 0;
};

// Decodes one packet at a time with the Helix MP3 or AAC decoder. Decoder
// state is heap-allocated in begin() and freed in end().
class AudioDecoder {
 public:
  AudioDecoder() = default;
  ~AudioDecoder() { end(); }
  AudioDecoder(const AudioDecoder&) = delete;
  AudioDecoder& operator=(const AudioDecoder&) = delete;

  // `aac` is used only for Codec::Aac. Returns false on allocation failure.
  bool begin(Codec codec, const AacConfig& aac);
  void end();
  // Clears overlap and bit-reservoir state after a seek. For MP3 this
  // reallocates the decoder; returns false if that allocation failed.
  bool flush();

  // Decodes one packet into interleaved PCM (`capacity` >= MAX_DECODED_SAMPLES).
  // Returns frames per channel; 0 when the packet yields no audio yet (MP3
  // right after a seek); <0 for a damaged packet, which callers skip.
  int decode(const uint8_t* packet, size_t len, int16_t* pcm, size_t capacity, PcmFormat& format);

 private:
  void* mp3_ = nullptr;
  void* aac_ = nullptr;
};

}  // namespace audiobook
