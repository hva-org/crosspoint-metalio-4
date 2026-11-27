#pragma once

#include <memory>

#include "ByteSource.h"
#include "TrackReader.h"

namespace audiobook {

// AudioSpecificConfig fields the player needs, plus the raw config for the
// decoder. 14 bytes cover LC with explicit or backward-compatible SBR and PS
// signalling (7 bytes at most) and keep the struct, which travels in every
// ring message, at 24 bytes.
struct AacConfig {
  static constexpr size_t MAX_BYTES = 14;

  uint8_t objectType = 0;  // 2 = LC; 5/29 = HE-AAC (SBR) over an LC core
  uint32_t coreRate = 0;
  uint8_t channels = 0;
  uint8_t length = 0;
  uint8_t bytes[MAX_BYTES] = {};
};

// Parses an AudioSpecificConfig. Returns false for unsupported object types
// and for configs longer than AacConfig::MAX_BYTES.
bool parseAacConfig(const uint8_t* data, size_t len, AacConfig& out);

// Sample tables of one track, located but not loaded.
struct Mp4TrackInfo {
  uint32_t trackId = 0;
  uint32_t handler = 0;  // 'soun', 'text', ...
  uint32_t timescale = 0;
  uint64_t duration = 0;
  uint32_t chapterTrackId = 0;  // from tref/chap
  uint64_t sttsOffset = 0;
  uint32_t sttsCount = 0;
  uint64_t stscOffset = 0;
  uint32_t stscCount = 0;
  uint64_t stszOffset = 0;
  uint32_t sampleCount = 0;
  uint32_t constantSampleSize = 0;
  uint64_t chunkOffsetsOffset = 0;
  uint32_t chunkCount = 0;
  bool chunkOffsets64 = false;
  // Audio sample entry
  bool isAac = false;
  bool isMp3 = false;
  uint16_t entryChannels = 0;
  uint32_t entrySampleRate = 0;
  AacConfig aac;
};

// Walks one track's samples in order, reading its tables through small
// windows so a long book's tables never sit in RAM.
class Mp4SampleIterator {
 public:
  // Allocates the table windows. Returns false on allocation failure.
  bool init(ByteSource* source, const Mp4TrackInfo& track);

  bool seekSample(uint32_t sample);
  // Positions at the sample that contains `units` (track timescale).
  bool seekTime(uint64_t units);
  bool advance();
  bool atEnd() const { return sample_ >= track_.sampleCount; }

  uint64_t offset() const { return offset_; }
  uint64_t time() const { return time_; }
  bool size(uint32_t& out) { return sampleSize(sample_, out); }

 private:
  class Table {
   public:
    void attach(uint64_t offset, uint32_t count, uint8_t entrySize, uint8_t* window, uint32_t windowEntries);
    // Points at entry `i`, loading its window. Invalidated by the next call.
    const uint8_t* entry(ByteSource& source, uint32_t i);
    uint32_t count() const { return count_; }

   private:
    uint64_t offset_ = 0;
    uint32_t count_ = 0;
    uint8_t entrySize_ = 0;
    uint8_t* window_ = nullptr;
    uint32_t capacity_ = 0;
    uint32_t first_ = 0;
    uint32_t loaded_ = 0;
  };

  bool sampleSize(uint32_t sample, uint32_t& out);
  bool chunkOffset(uint32_t chunk, uint64_t& out);
  bool loadStsc(uint32_t entry);

  ByteSource* source_ = nullptr;
  Mp4TrackInfo track_;
  std::unique_ptr<uint8_t[]> windows_;
  Table stts_, stsc_, stsz_, stco_;

  uint32_t sample_ = 0;
  uint64_t offset_ = 0;
  uint64_t time_ = 0;
  uint32_t sttsEntry_ = 0;
  uint32_t sttsLeft_ = 0;
  uint32_t sttsDelta_ = 0;
  uint32_t stscEntry_ = 0;
  uint32_t samplesPerChunk_ = 0;
  uint32_t nextRunChunk_ = 0;  // first chunk of the next stsc run (1-based)
  uint32_t chunk_ = 0;         // 1-based
  uint32_t leftInChunk_ = 0;
};

// MP4/M4A/M4B reader for the first AAC (or MP3) audio track, with chapters
// from Nero `chpl` or a QuickTime chapter text track.
class Mp4Reader final : public TrackReader {
 public:
  static constexpr size_t MAX_CHAPTERS = 512;

  // `cache` must outlive the reader and be initialized over `source`.
  // Without `withChapters`, chapter lists are not read or allocated.
  bool open(ByteSource& source, CachedReader& cache, bool withChapters = true);

  Codec codec() const override { return audio_.isMp3 ? Codec::Mp3 : Codec::Aac; }
  uint32_t sampleRate() const override;
  uint8_t channels() const override;
  uint64_t durationMs() const override;
  const AacConfig& aacConfig() const { return audio_.aac; }

  bool seekMs(uint64_t ms, uint64_t& actualMs) override;
  int nextPacket(uint8_t* dst, size_t capacity, uint64_t& startMs) override;

  size_t chapterCount() const { return chapterCount_; }
  const Chapter* chapters() const { return chapters_.get(); }

 private:
  bool parseMoov(uint64_t start, uint64_t end);
  bool parseTrak(uint64_t start, uint64_t end, Mp4TrackInfo& track);
  bool parseStbl(uint64_t start, uint64_t end, Mp4TrackInfo& track);
  bool parseStsd(uint64_t start, uint64_t end, Mp4TrackInfo& track);
  bool parseSampleEntryChildren(uint64_t start, uint64_t end, Mp4TrackInfo& track, int depth);
  bool parseEsds(uint64_t start, uint64_t end, Mp4TrackInfo& track);
  void parseChpl(uint64_t start, uint64_t end);
  void loadTextChapters(const Mp4TrackInfo& textTrack);
  bool allocateChapters(size_t count);

  ByteSource* source_ = nullptr;
  CachedReader* cache_ = nullptr;
  Mp4TrackInfo audio_;
  bool haveAudio_ = false;
  Mp4SampleIterator samples_;

  // A text track can precede or follow the audio track it serves.
  static constexpr size_t MAX_TEXT_TRACKS = 4;
  Mp4TrackInfo textTracks_[MAX_TEXT_TRACKS];
  size_t textTrackCount_ = 0;

  std::unique_ptr<Chapter[]> chapters_;
  size_t chapterCount_ = 0;
  uint64_t chplStart_ = 0;
  uint64_t chplEnd_ = 0;
};

}  // namespace audiobook
