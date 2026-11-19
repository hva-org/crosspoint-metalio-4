#include "AudioDecoder.h"

#include <libhelix-aac/aacdec.h>
#include <libhelix-mp3/mp3dec.h>

namespace audiobook {

bool AudioDecoder::begin(const Codec codec, const AacConfig& aac) {
  end();
  if (codec == Codec::Mp3) {
    mp3_ = MP3InitDecoder();
    return mp3_ != nullptr;
  }

  aac_ = AACInitDecoder();
  if (!aac_) return false;
  // MP4 carries raw AAC blocks without ADTS headers, so the stream format
  // comes from the AudioSpecificConfig. Helix detects SBR in the blocks.
  AACFrameInfo info = {};
  info.nChans = aac.channels;
  info.sampRateCore = static_cast<int>(aac.coreRate);
  info.profile = AAC_PROFILE_LC;
  if (AACSetRawBlockParams(static_cast<HAACDecoder>(aac_), 0, &info) != ERR_AAC_NONE) {
    end();
    return false;
  }
  return true;
}

void AudioDecoder::end() {
  if (mp3_) MP3FreeDecoder(static_cast<HMP3Decoder>(mp3_));
  if (aac_) AACFreeDecoder(static_cast<HAACDecoder>(aac_));
  mp3_ = nullptr;
  aac_ = nullptr;
}

bool AudioDecoder::flush() {
  if (aac_) return AACFlushCodec(static_cast<HAACDecoder>(aac_)) == ERR_AAC_NONE;
  if (!mp3_) return false;
  // Helix MP3 has no flush: its bit reservoir would feed stale data into the
  // first frame after a seek. A fresh decoder starts clean.
  MP3FreeDecoder(static_cast<HMP3Decoder>(mp3_));
  mp3_ = MP3InitDecoder();
  return mp3_ != nullptr;
}

int AudioDecoder::decode(const uint8_t* packet, const size_t len, int16_t* pcm, const size_t capacity,
                         PcmFormat& format) {
  if (capacity < MAX_DECODED_SAMPLES || len == 0) return -1;
  // Helix takes a non-const cursor but only reads the input.
  auto* in = const_cast<unsigned char*>(packet);
  int left = static_cast<int>(len);

  if (mp3_) {
    const int err = MP3Decode(static_cast<HMP3Decoder>(mp3_), &in, &left, pcm, 0);
    if (err == ERR_MP3_MAINDATA_UNDERFLOW) return 0;
    if (err != ERR_MP3_NONE) return -1;
    MP3FrameInfo info;
    MP3GetLastFrameInfo(static_cast<HMP3Decoder>(mp3_), &info);
    if (info.nChans <= 0) return -1;
    format.sampleRate = static_cast<uint32_t>(info.samprate);
    format.channels = static_cast<uint8_t>(info.nChans);
    return info.outputSamps / info.nChans;
  }

  if (aac_) {
    if (AACDecode(static_cast<HAACDecoder>(aac_), &in, &left, pcm) != ERR_AAC_NONE) return -1;
    AACFrameInfo info;
    AACGetLastFrameInfo(static_cast<HAACDecoder>(aac_), &info);
    if (info.nChans <= 0) return -1;
    format.sampleRate = static_cast<uint32_t>(info.sampRateOut);
    format.channels = static_cast<uint8_t>(info.nChans);
    return info.outputSamps / info.nChans;
  }
  return -1;
}

}  // namespace audiobook
