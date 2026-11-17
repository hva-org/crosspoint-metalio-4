#pragma once

#include "ByteSource.h"
#include "TrackReader.h"

namespace audiobook {

struct Mp3FrameHeader {
  uint32_t sampleRate = 0;
  uint32_t bitrate = 0;  // bits per second
  uint16_t samplesPerFrame = 0;
  uint16_t length = 0;  // bytes, including the 4-byte header
  uint8_t channels = 0;
  uint8_t version = 0;  // 1 = MPEG-1, 2 = MPEG-2, 25 = MPEG-2.5
};

// Parses an MPEG audio Layer III frame header. Free-format and Layer I/II
// frames are rejected: the Helix decoder handles Layer III only.
bool parseMp3Header(const uint8_t* h, Mp3FrameHeader& out);

// Frame-by-frame MP3 reader: skips ID3v2 tags and stops at an ID3v1 tag.
// Xing/Info TOCs give VBR seeking; otherwise seeks use the average bitrate.
class Mp3Reader final : public TrackReader {
 public:
  // Parses headers. `cache` must outlive the reader and be initialized.
  bool open(CachedReader& cache);

  Codec codec() const override { return Codec::Mp3; }
  uint32_t sampleRate() const override { return first_.sampleRate; }
  uint8_t channels() const override { return first_.channels; }
  uint64_t durationMs() const override { return durationMs_; }

  bool seekMs(uint64_t ms, uint64_t& actualMs) override;
  int nextPacket(uint8_t* dst, size_t capacity, uint64_t& startMs) override;

 private:
  // Finds a frame at or after `pos` whose successor is also a frame header,
  // so a stray 0xFFE pattern in data is not taken as sync.
  bool findFrame(uint64_t pos, uint64_t limit, uint64_t& found, Mp3FrameHeader& header);
  bool frameAt(uint64_t pos, Mp3FrameHeader& header);
  // Returns the position after any ID3v2 tags starting at `pos`.
  uint64_t skipId3(uint64_t pos);
  uint64_t frameStartMs(uint64_t frameIndex) const;

  CachedReader* cache_ = nullptr;
  Mp3FrameHeader first_;
  uint64_t audioStart_ = 0;  // first audio frame (after any Xing frame)
  uint64_t audioEnd_ = 0;    // excludes a trailing ID3v1 tag
  uint64_t totalFrames_ = 0;
  uint64_t durationMs_ = 0;
  uint8_t toc_[100] = {};
  bool hasToc_ = false;

  uint64_t position_ = 0;
  uint64_t frameIndex_ = 0;
};

}  // namespace audiobook
