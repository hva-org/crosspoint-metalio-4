#include "Mp3Reader.h"

#include <cstring>

namespace audiobook {
namespace {
constexpr uint16_t BITRATES_MPEG1[15] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
constexpr uint16_t BITRATES_MPEG2[15] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
constexpr uint32_t SAMPLE_RATES[3][3] = {{44100, 48000, 32000}, {22050, 24000, 16000}, {11025, 12000, 8000}};
// How far to look for frame sync after a tag, a seek, or damaged data.
constexpr uint64_t SYNC_SEARCH_BYTES = 64 * 1024;
constexpr size_t ID3V1_BYTES = 128;
constexpr size_t APE_FOOTER_BYTES = 32;
constexpr int MAX_ID3V2_TAGS = 4;
}  // namespace

bool parseMp3Header(const uint8_t* h, Mp3FrameHeader& out) {
  if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return false;
  const uint8_t versionBits = (h[1] >> 3) & 3;
  const uint8_t layerBits = (h[1] >> 1) & 3;
  const uint8_t bitrateIndex = h[2] >> 4;
  const uint8_t rateIndex = (h[2] >> 2) & 3;
  // versionBits 1 is reserved; layerBits 1 is Layer III.
  if (versionBits == 1 || layerBits != 1 || bitrateIndex == 0 || bitrateIndex == 15 || rateIndex == 3) return false;

  const bool mpeg1 = versionBits == 3;
  const int row = mpeg1 ? 0 : versionBits == 2 ? 1 : 2;
  out.version = mpeg1 ? 1 : versionBits == 2 ? 2 : 25;
  out.sampleRate = SAMPLE_RATES[row][rateIndex];
  out.bitrate = static_cast<uint32_t>((mpeg1 ? BITRATES_MPEG1 : BITRATES_MPEG2)[bitrateIndex]) * 1000u;
  out.samplesPerFrame = mpeg1 ? 1152 : 576;
  const uint32_t padding = (h[2] >> 1) & 1;
  out.length = static_cast<uint16_t>((mpeg1 ? 144u : 72u) * out.bitrate / out.sampleRate + padding);
  out.channels = ((h[3] >> 6) & 3) == 3 ? 1 : 2;
  return true;
}

bool Mp3Reader::frameAt(const uint64_t pos, Mp3FrameHeader& header) {
  const uint8_t* p = nullptr;
  return cache_->peekAt(pos, 4, &p) == 4 && parseMp3Header(p, header);
}

bool Mp3Reader::findFrame(const uint64_t pos, const uint64_t limit, uint64_t& found, Mp3FrameHeader& header) {
  for (uint64_t p = pos; p < limit && p + 4 <= audioEnd_; p++) {
    Mp3FrameHeader h;
    if (!frameAt(p, h)) continue;
    const uint64_t next = p + h.length;
    if (next > audioEnd_) continue;
    Mp3FrameHeader following;
    // The last frame of the file has no successor to confirm it.
    const bool confirmed = next + 4 > audioEnd_ || (frameAt(next, following) && following.sampleRate == h.sampleRate &&
                                                    following.version == h.version);
    if (confirmed) {
      found = p;
      header = h;
      return true;
    }
  }
  return false;
}

uint64_t Mp3Reader::skipId3(uint64_t pos) {
  const uint8_t* p = nullptr;
  for (int i = 0; i < MAX_ID3V2_TAGS; i++) {
    if (cache_->peekAt(pos, 10, &p) < 10 || std::memcmp(p, "ID3", 3) != 0) break;
    const uint32_t size = static_cast<uint32_t>(p[6] & 0x7F) << 21 | static_cast<uint32_t>(p[7] & 0x7F) << 14 |
                          static_cast<uint32_t>(p[8] & 0x7F) << 7 | (p[9] & 0x7F);
    const bool hasFooter = (p[5] & 0x10) != 0;
    pos += 10 + static_cast<uint64_t>(size) + (hasFooter ? 10 : 0);
  }
  return pos;
}

bool Mp3Reader::open(CachedReader& cache) {
  cache_ = &cache;
  const uint64_t total = cache.size();
  audioEnd_ = total;
  const uint8_t* p = nullptr;

  const uint64_t pos = skipId3(0);
  if (total >= ID3V1_BYTES && cache.peekAt(total - ID3V1_BYTES, 3, &p) == 3 && std::memcmp(p, "TAG", 3) == 0) {
    audioEnd_ = total - ID3V1_BYTES;
  }
  // An APEv2 tag (often cover art) sits before any ID3v1 tag; its footer
  // gives the size of everything after the audio.
  if (audioEnd_ >= APE_FOOTER_BYTES &&
      cache.peekAt(audioEnd_ - APE_FOOTER_BYTES, APE_FOOTER_BYTES, &p) == APE_FOOTER_BYTES &&
      std::memcmp(p, "APETAGEX", 8) == 0) {
    const auto le32 = [](const uint8_t* b) {
      return static_cast<uint32_t>(b[0]) | static_cast<uint32_t>(b[1]) << 8 | static_cast<uint32_t>(b[2]) << 16 |
             static_cast<uint32_t>(b[3]) << 24;
    };
    const uint64_t tagBytes = static_cast<uint64_t>(le32(p + 12)) + ((le32(p + 20) & 0x80000000u) ? 32 : 0);
    if (tagBytes <= audioEnd_) audioEnd_ -= tagBytes;
  }

  uint64_t framePos = 0;
  if (!findFrame(pos, pos + SYNC_SEARCH_BYTES, framePos, first_)) return false;
  audioStart_ = framePos;

  // A Xing/Info or VBRI frame carries no audio; it describes the stream.
  const size_t sideInfo = first_.version == 1 ? (first_.channels == 1 ? 17 : 32) : (first_.channels == 1 ? 9 : 17);
  const size_t xing = 4 + sideInfo;
  const size_t available = cache.peekAt(framePos, first_.length, &p);
  if (available >= xing + 8 && (std::memcmp(p + xing, "Xing", 4) == 0 || std::memcmp(p + xing, "Info", 4) == 0)) {
    const uint32_t flags = be32(p + xing + 4);
    size_t q = xing + 8;
    if ((flags & 0x1) && q + 4 <= available) {
      totalFrames_ = be32(p + q);
      q += 4;
    }
    if (flags & 0x2) q += 4;
    if ((flags & 0x4) && q + sizeof(toc_) <= available) {
      std::memcpy(toc_, p + q, sizeof(toc_));
      hasToc_ = true;
    }
    audioStart_ = framePos + first_.length;
  } else if (available >= 36 + 18 && std::memcmp(p + 36, "VBRI", 4) == 0) {
    totalFrames_ = be32(p + 36 + 14);
    audioStart_ = framePos + first_.length;
  }

  if (totalFrames_ > 0) {
    durationMs_ = totalFrames_ * first_.samplesPerFrame * 1000 / first_.sampleRate;
  } else {
    durationMs_ = (audioEnd_ - audioStart_) * 8000 / first_.bitrate;
  }
  position_ = audioStart_;
  frameIndex_ = 0;
  return true;
}

uint64_t Mp3Reader::frameStartMs(const uint64_t frameIndex) const {
  return frameIndex * first_.samplesPerFrame * 1000 / first_.sampleRate;
}

bool Mp3Reader::seekMs(const uint64_t ms, uint64_t& actualMs) {
  if (!cache_) return false;
  if (ms >= durationMs_) {
    position_ = audioEnd_;
    actualMs = durationMs_;
    return true;
  }

  const uint64_t audioBytes = audioEnd_ - audioStart_;
  uint64_t target = 0;
  if (hasToc_ && durationMs_ > 0) {
    // The TOC maps each whole percent of time to a byte fraction /256.
    const double percent = static_cast<double>(ms) * 100.0 / static_cast<double>(durationMs_);
    const int i = percent < 99.0 ? static_cast<int>(percent) : 99;
    const double a = toc_[i];
    const double b = i < 99 ? toc_[i + 1] : 256.0;
    const double fraction = (a + (b - a) * (percent - i)) / 256.0;
    target = audioStart_ + static_cast<uint64_t>(fraction * static_cast<double>(audioBytes));
  } else if (totalFrames_ > 0 && durationMs_ > 0) {
    // Known length but no TOC: the average bitrate beats the first frame's.
    target = audioStart_ + audioBytes * ms / durationMs_;
  } else {
    target = audioStart_ + ms * first_.bitrate / 8000;
  }
  if (target >= audioEnd_) target = audioEnd_;

  uint64_t found = 0;
  Mp3FrameHeader header;
  if (!findFrame(target, target + SYNC_SEARCH_BYTES, found, header)) return false;
  position_ = found;
  const uint64_t spf = first_.samplesPerFrame;
  frameIndex_ = (ms * first_.sampleRate / 1000 + spf / 2) / spf;
  actualMs = frameStartMs(frameIndex_);
  return true;
}

int Mp3Reader::nextPacket(uint8_t* dst, const size_t capacity, uint64_t& startMs) {
  if (!cache_ || position_ >= audioEnd_) return 0;
  Mp3FrameHeader header;
  if (!frameAt(position_, header) || header.sampleRate != first_.sampleRate) {
    // Merged audiobooks can carry a tag (often cover art) between tracks.
    position_ = skipId3(position_);
    if (position_ >= audioEnd_) return 0;
    uint64_t found = 0;
    if (!frameAt(position_, header) || header.sampleRate != first_.sampleRate) {
      if (!findFrame(position_ + 1, position_ + SYNC_SEARCH_BYTES, found, header)) {
        // Trailing junk ends the track; lost sync with audio left is an error.
        // Past the header's frame count the rest is taken as junk. That count
        // is only a hint: after a seek frameIndex_ is estimated, and merged
        // files keep their first part's count.
        const bool pastCount = totalFrames_ > 0 && frameIndex_ >= totalFrames_;
        return pastCount || audioEnd_ - position_ <= SYNC_SEARCH_BYTES ? 0 : -1;
      }
      position_ = found;
    }
  }
  if (position_ + header.length > audioEnd_) return 0;
  if (header.length > capacity) return -1;
  if (!cache_->readAt(position_, dst, header.length)) return -1;
  startMs = frameStartMs(frameIndex_);
  position_ += header.length;
  frameIndex_++;
  return header.length;
}

}  // namespace audiobook
