#include "AudioDecoder.h"

#include <Memory.h>
#include <minimp3.h>
#include <neaacdec.h>

#include <cstring>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

namespace audiobook {

// About 6.7 KB, so it lives on the heap rather than the decoder task's stack.
struct AudioDecoder::Mp3State {
  mp3dec_t decoder;
};

namespace {
constexpr uint8_t AOT_SBR = 5;
constexpr uint8_t AOT_SBR_PS = 29;

// faad2 writes to its PSRAM blocks without checking the allocation, so an AAC
// track starts only when its state fits: about 120 KB in total. The largest
// free block must hold all of it, since the heap may carve every block from
// it; the free-size margin covers other tasks' use.
constexpr size_t AAC_PSRAM_FREE = 192 * 1024;
constexpr size_t AAC_PSRAM_BLOCK = 128 * 1024;

bool psramFitsAac() {
#ifdef ESP_PLATFORM
  return heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= AAC_PSRAM_FREE &&
         heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= AAC_PSRAM_BLOCK;
#else
  return true;  // host builds allocate with plain malloc
#endif
}
}  // namespace

AudioDecoder::AudioDecoder() = default;
AudioDecoder::~AudioDecoder() { end(); }

bool AudioDecoder::begin(const Codec codec, const AacConfig& aac) {
  end();
  if (codec == Codec::Mp3) {
    mp3_ = makeUniqueNoThrow<Mp3State>();
    if (!mp3_) return false;
    mp3dec_init(&mp3_->decoder);
    return true;
  }

  if (aac.length > AacConfig::MAX_BYTES || !psramFitsAac()) return false;
  uint8_t config[AacConfig::MAX_BYTES];
  memcpy(config, aac.bytes, aac.length);
  // faad2 is built without parametric stereo and rejects object type 29. Such
  // streams carry an HE-AAC v1 mono downmix, which is what the mono output needs.
  if (aac.length > 0 && config[0] >> 3 == AOT_SBR_PS) {
    config[0] = static_cast<uint8_t>(AOT_SBR << 3 | (config[0] & 0x07));
  }

  aac_ = NeAACDecOpen();
  if (!aac_) return false;
  // This is the decoder's live configuration: SetConfiguration validates it
  // but does not copy dontUpSampleImplicitSBR.
  NeAACDecConfigurationPtr settings = NeAACDecGetCurrentConfiguration(aac_);
  settings->defObjectType = LC;
  settings->outputFormat = FAAD_FMT_16BIT;
  // A stream without SBR data plays at its stored rate instead of upsampled.
  settings->dontUpSampleImplicitSBR = 1;
  unsigned long rate = 0;
  unsigned char channels = 0;
  if (!NeAACDecSetConfiguration(aac_, settings) || NeAACDecInit2(aac_, config, aac.length, &rate, &channels) != 0) {
    end();
    return false;
  }
  return true;
}

void AudioDecoder::end() {
  mp3_.reset();
  if (aac_) NeAACDecClose(aac_);
  aac_ = nullptr;
}

bool AudioDecoder::flush() {
  if (aac_) {
    // Frame 0 makes faad2 drop the next frame, whose overlap still holds audio
    // from before the seek, and resets SBR.
    NeAACDecPostSeekReset(aac_, 0);
    return true;
  }
  if (!mp3_) return false;
  // The next frame then clears all state, including the bit reservoir.
  mp3dec_init(&mp3_->decoder);
  return true;
}

int AudioDecoder::decode(const uint8_t* packet, const size_t len, int16_t* pcm, const size_t capacity,
                         PcmFormat& format) {
  if (capacity < MAX_DECODED_SAMPLES || len == 0 || len > MAX_PACKET_BYTES) return -1;

  if (mp3_) {
    mp3dec_frame_info_t info = {};
    const int frames = mp3dec_decode_frame(&mp3_->decoder, packet, static_cast<int>(len), pcm, &info);
    if (frames > 0) {
      format.sampleRate = static_cast<uint32_t>(info.hz);
      format.channels = static_cast<uint8_t>(info.channels);
      return frames;
    }
    // minimp3 clears its saved header when it finds no frame or a damaged
    // one. A Layer III frame that keeps it was short of bit-reservoir data.
    return mp3_->decoder.header[0] == 0xFF && info.layer == 3 ? 0 : -1;
  }

  if (aac_) {
    NeAACDecFrameInfo info;
    void* out = pcm;
    // faad2 takes a non-const pointer but only reads the input. It refuses a
    // frame that does not fit in `capacity` rather than writing past it.
    NeAACDecDecode2(aac_, &info, const_cast<uint8_t*>(packet), len, &out, capacity * sizeof(int16_t));
    if (info.error != 0 || info.channels == 0) return -1;
    format.sampleRate = static_cast<uint32_t>(info.samplerate);
    format.channels = info.channels;
    return static_cast<int>(info.samples / info.channels);
  }
  return -1;
}

}  // namespace audiobook
