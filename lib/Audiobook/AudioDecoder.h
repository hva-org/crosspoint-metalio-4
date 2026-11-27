#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "Mp4Reader.h"
#include "TrackReader.h"

namespace audiobook {

// One HE-AAC frame: 2048 samples per channel, two channels.
inline constexpr size_t MAX_DECODED_SAMPLES = 2048 * 2;

struct PcmFormat {
  uint32_t sampleRate = 0;
  uint8_t channels = 0;
};

// Decodes one packet at a time: MP3 with minimp3, AAC-LC and HE-AAC with
// faad2. Decoder state is heap-allocated in begin() and freed in end().
class AudioDecoder {
 public:
  AudioDecoder();
  ~AudioDecoder();
  AudioDecoder(const AudioDecoder&) = delete;
  AudioDecoder& operator=(const AudioDecoder&) = delete;

  // `aac` is used only for Codec::Aac. Returns false on allocation failure, too
  // little PSRAM for faad2, or an AudioSpecificConfig the decoder rejects.
  bool begin(Codec codec, const AacConfig& aac);
  void end();
  // Clears overlap and bit-reservoir state after a seek. Returns false when
  // no decoder is open.
  bool flush();

  // Decodes one packet into interleaved PCM (`capacity` >= MAX_DECODED_SAMPLES).
  // Returns frames per channel; 0 when the packet yields no audio yet (the
  // first AAC frame after begin() or flush(), MP3 right after a seek); <0 for
  // a damaged packet, which callers skip.
  int decode(const uint8_t* packet, size_t len, int16_t* pcm, size_t capacity, PcmFormat& format);

 private:
  struct Mp3State;

  std::unique_ptr<Mp3State> mp3_;
  void* aac_ = nullptr;  // faad2 handle, closed in end()
};

}  // namespace audiobook
