#include "Mp4Reader.h"

#include <Memory.h>

#include <cstring>

namespace audiobook {
namespace {
constexpr uint32_t fourcc(const char (&s)[5]) {
  return static_cast<uint32_t>(s[0]) << 24 | static_cast<uint32_t>(s[1]) << 16 | static_cast<uint32_t>(s[2]) << 8 |
         static_cast<uint32_t>(s[3]);
}

constexpr uint32_t AAC_RATES[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                    22050, 16000, 12000, 11025, 8000,  7350};
// Table window sizes, in entries. 128 entries cover several seconds of AAC.
constexpr uint32_t WINDOW_ENTRIES = 128;
constexpr size_t WINDOW_BYTES = WINDOW_ENTRIES * (8 + 12 + 4 + 8);
constexpr int MAX_BOX_DEPTH = 4;
constexpr size_t MAX_CHAPTER_SAMPLE_BYTES = 2 + CHAPTER_TITLE_CAPACITY * 2;
// ESDS object types for MPEG-1/2 audio (MP3 carried in MP4).
constexpr uint8_t OTI_MPEG2_AUDIO = 0x69;
constexpr uint8_t OTI_MPEG1_AUDIO = 0x6B;

struct Box {
  uint32_t type = 0;
  uint64_t body = 0;
  uint64_t end = 0;
};

bool readBox(CachedReader& cache, const uint64_t pos, const uint64_t limit, Box& box) {
  if (pos + 8 > limit) return false;
  uint8_t h[16];
  if (!cache.readAt(pos, h, 8)) return false;
  uint64_t size = be32(h);
  uint64_t header = 8;
  box.type = be32(h + 4);
  if (size == 1) {
    if (pos + 16 > limit || !cache.readAt(pos + 8, h + 8, 8)) return false;
    size = be64(h + 8);
    header = 16;
  } else if (size == 0) {
    size = limit - pos;
  }
  if (size < header || size > limit - pos) return false;
  box.body = pos + header;
  box.end = pos + size;
  return true;
}

// Reads an MPEG-4 descriptor length (1-4 bytes, 7 bits each).
bool readDescriptorLength(const uint8_t* p, const size_t avail, size_t& pos, uint32_t& length) {
  length = 0;
  for (int i = 0; i < 4; i++) {
    if (pos >= avail) return false;
    const uint8_t b = p[pos++];
    length = length << 7 | (b & 0x7F);
    if (!(b & 0x80)) return true;
  }
  return true;
}

class BitReader {
 public:
  BitReader(const uint8_t* data, const size_t len) : data_(data), bits_(len * 8) {}
  bool read(const int count, uint32_t& out) {
    out = 0;
    for (int i = 0; i < count; i++) {
      if (pos_ >= bits_) return false;
      out = out << 1 | ((data_[pos_ / 8] >> (7 - pos_ % 8)) & 1);
      pos_++;
    }
    return true;
  }

 private:
  const uint8_t* data_;
  size_t bits_;
  size_t pos_ = 0;
};

bool readObjectType(BitReader& bits, uint32_t& type) {
  if (!bits.read(5, type)) return false;
  if (type == 31) {
    uint32_t ext = 0;
    if (!bits.read(6, ext)) return false;
    type = 32 + ext;
  }
  return true;
}

bool readRate(BitReader& bits, uint32_t& rate) {
  uint32_t index = 0;
  if (!bits.read(4, index)) return false;
  if (index == 15) return bits.read(24, rate);
  if (index >= 13) return false;
  rate = AAC_RATES[index];
  return true;
}
}  // namespace

bool parseAacConfig(const uint8_t* data, const size_t len, AacConfig& out) {
  BitReader bits(data, len);
  uint32_t type = 0, rate = 0, channels = 0;
  if (!readObjectType(bits, type) || !readRate(bits, rate) || !bits.read(4, channels)) return false;
  out.objectType = static_cast<uint8_t>(type);
  out.coreRate = rate;
  out.channels = static_cast<uint8_t>(channels);
  if (type == 5 || type == 29) {
    // Explicit HE-AAC: the extension rate, then the core object type.
    uint32_t extRate = 0, core = 0;
    if (!readRate(bits, extRate) || !readObjectType(bits, core)) return false;
    if (core != 2) return false;
    return true;
  }
  return type == 2;
}

// ---------------------------------------------------------------------------
// Sample iterator

void Mp4SampleIterator::Table::attach(const uint64_t offset, const uint32_t count, const uint8_t entrySize,
                                      uint8_t* window, const uint32_t windowEntries) {
  offset_ = offset;
  count_ = count;
  entrySize_ = entrySize;
  window_ = window;
  capacity_ = windowEntries;
  first_ = 0;
  loaded_ = 0;
}

const uint8_t* Mp4SampleIterator::Table::entry(ByteSource& source, const uint32_t i) {
  if (i >= count_) return nullptr;
  if (i < first_ || i >= first_ + loaded_) {
    const uint32_t n = count_ - i < capacity_ ? count_ - i : capacity_;
    if (!readExact(source, offset_ + static_cast<uint64_t>(i) * entrySize_, window_,
                   static_cast<size_t>(n) * entrySize_)) {
      loaded_ = 0;
      return nullptr;
    }
    first_ = i;
    loaded_ = n;
  }
  return window_ + static_cast<size_t>(i - first_) * entrySize_;
}

bool Mp4SampleIterator::init(ByteSource* source, const Mp4TrackInfo& track) {
  source_ = source;
  track_ = track;
  if (!windows_) windows_ = makeUniqueNoThrow<uint8_t[]>(WINDOW_BYTES);
  if (!windows_) return false;
  uint8_t* w = windows_.get();
  stts_.attach(track.sttsOffset, track.sttsCount, 8, w, WINDOW_ENTRIES);
  w += WINDOW_ENTRIES * 8;
  stsc_.attach(track.stscOffset, track.stscCount, 12, w, WINDOW_ENTRIES);
  w += WINDOW_ENTRIES * 12;
  stsz_.attach(track.stszOffset, track.constantSampleSize ? 0 : track.sampleCount, 4, w, WINDOW_ENTRIES);
  w += WINDOW_ENTRIES * 4;
  stco_.attach(track.chunkOffsetsOffset, track.chunkCount, track.chunkOffsets64 ? 8 : 4, w, WINDOW_ENTRIES);
  return seekSample(0);
}

bool Mp4SampleIterator::sampleSize(const uint32_t sample, uint32_t& out) {
  if (track_.constantSampleSize) {
    out = track_.constantSampleSize;
    return true;
  }
  const uint8_t* e = stsz_.entry(*source_, sample);
  if (!e) return false;
  out = be32(e);
  return true;
}

bool Mp4SampleIterator::chunkOffset(const uint32_t chunk, uint64_t& out) {
  if (chunk == 0) return false;
  const uint8_t* e = stco_.entry(*source_, chunk - 1);
  if (!e) return false;
  out = track_.chunkOffsets64 ? be64(e) : be32(e);
  return true;
}

bool Mp4SampleIterator::loadStsc(const uint32_t entry) {
  const uint8_t* e = stsc_.entry(*source_, entry);
  if (!e) return false;
  const uint32_t first = be32(e);
  samplesPerChunk_ = be32(e + 4);
  nextRunChunk_ = track_.chunkCount + 1;
  if (entry + 1 < stsc_.count()) {
    const uint8_t* next = stsc_.entry(*source_, entry + 1);
    if (!next) return false;
    nextRunChunk_ = be32(next);
  }
  stscEntry_ = entry;
  return samplesPerChunk_ > 0 && first > 0 && nextRunChunk_ > first;
}

bool Mp4SampleIterator::seekSample(const uint32_t sample) {
  sample_ = sample;
  if (sample >= track_.sampleCount) return true;

  // Time: walk the stts runs.
  uint64_t before = 0, time = 0;
  bool found = false;
  for (uint32_t i = 0; i < stts_.count(); i++) {
    const uint8_t* e = stts_.entry(*source_, i);
    if (!e) return false;
    const uint32_t count = be32(e);
    const uint32_t delta = be32(e + 4);
    if (sample < before + count) {
      sttsEntry_ = i;
      sttsLeft_ = static_cast<uint32_t>(before + count - sample);
      sttsDelta_ = delta;
      time_ = time + (sample - before) * delta;
      found = true;
      break;
    }
    before += count;
    time += static_cast<uint64_t>(count) * delta;
  }
  if (!found) return false;

  // Chunk: walk the stsc runs.
  before = 0;
  found = false;
  for (uint32_t i = 0; i < stsc_.count(); i++) {
    if (!loadStsc(i)) return false;
    const uint8_t* e = stsc_.entry(*source_, i);
    if (!e) return false;
    const uint32_t first = be32(e);
    const uint64_t runSamples = static_cast<uint64_t>(nextRunChunk_ - first) * samplesPerChunk_;
    if (sample < before + runSamples) {
      const uint64_t index = sample - before;
      chunk_ = first + static_cast<uint32_t>(index / samplesPerChunk_);
      const uint32_t inChunk = static_cast<uint32_t>(index % samplesPerChunk_);
      leftInChunk_ = samplesPerChunk_ - inChunk;
      if (!chunkOffset(chunk_, offset_)) return false;
      for (uint32_t s = sample - inChunk; s < sample; s++) {
        uint32_t bytes = 0;
        if (!sampleSize(s, bytes)) return false;
        offset_ += bytes;
      }
      found = true;
      break;
    }
    before += runSamples;
  }
  return found;
}

bool Mp4SampleIterator::seekTime(const uint64_t units) {
  uint64_t before = 0, time = 0;
  for (uint32_t i = 0; i < stts_.count(); i++) {
    const uint8_t* e = stts_.entry(*source_, i);
    if (!e) return false;
    const uint32_t count = be32(e);
    const uint32_t delta = be32(e + 4);
    const uint64_t span = static_cast<uint64_t>(count) * delta;
    if (delta > 0 && units < time + span) {
      return seekSample(static_cast<uint32_t>(before + (units - time) / delta));
    }
    before += count;
    time += span;
  }
  return seekSample(track_.sampleCount);
}

bool Mp4SampleIterator::advance() {
  if (atEnd()) return false;
  uint32_t bytes = 0;
  if (!sampleSize(sample_, bytes)) return false;
  offset_ += bytes;
  sample_++;
  if (atEnd()) return true;

  time_ += sttsDelta_;
  while (--sttsLeft_ == 0 || sttsLeft_ == UINT32_MAX) {
    if (++sttsEntry_ >= stts_.count()) return false;
    const uint8_t* e = stts_.entry(*source_, sttsEntry_);
    if (!e) return false;
    sttsLeft_ = be32(e) + 1;  // pre-incremented for the loop test
    sttsDelta_ = be32(e + 4);
  }

  if (--leftInChunk_ == 0) {
    chunk_++;
    if (chunk_ >= nextRunChunk_ && stscEntry_ + 1 < stsc_.count() && !loadStsc(stscEntry_ + 1)) return false;
    leftInChunk_ = samplesPerChunk_;
    if (!chunkOffset(chunk_, offset_)) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Box parsing

bool Mp4Reader::open(ByteSource& source, CachedReader& cache, const bool withChapters) {
  source_ = &source;
  cache_ = &cache;
  const uint64_t total = cache.size();
  for (uint64_t pos = 0; pos + 8 <= total;) {
    Box box;
    if (!readBox(cache, pos, total, box)) break;
    if (box.type == fourcc("moov")) {
      if (!parseMoov(box.body, box.end)) return false;
      break;
    }
    pos = box.end;
  }
  if (!haveAudio_ || audio_.timescale == 0 || audio_.sampleCount == 0) return false;
  if (!samples_.init(&source, audio_)) return false;
  if (!withChapters) return true;

  if (chplEnd_ > chplStart_) parseChpl(chplStart_, chplEnd_);
  if (chapterCount_ == 0 && audio_.chapterTrackId != 0) {
    for (size_t i = 0; i < textTrackCount_; i++) {
      if (textTracks_[i].trackId == audio_.chapterTrackId) loadTextChapters(textTracks_[i]);
    }
  }
  return true;
}

bool Mp4Reader::parseMoov(const uint64_t start, const uint64_t end) {
  for (uint64_t pos = start; pos + 8 <= end;) {
    Box box;
    if (!readBox(*cache_, pos, end, box)) return false;
    if (box.type == fourcc("trak")) {
      Mp4TrackInfo track;
      if (parseTrak(box.body, box.end, track)) {
        if (track.handler == fourcc("soun") && (track.isAac || track.isMp3) && !haveAudio_) {
          audio_ = track;
          haveAudio_ = true;
        } else if (track.handler == fourcc("text") && textTrackCount_ < MAX_TEXT_TRACKS) {
          textTracks_[textTrackCount_++] = track;
        }
      }
    } else if (box.type == fourcc("udta")) {
      for (uint64_t p = box.body; p + 8 <= box.end;) {
        Box child;
        if (!readBox(*cache_, p, box.end, child)) break;
        if (child.type == fourcc("chpl")) {
          chplStart_ = child.body;
          chplEnd_ = child.end;
        }
        p = child.end;
      }
    }
    pos = box.end;
  }
  return true;
}

bool Mp4Reader::parseTrak(const uint64_t start, const uint64_t end, Mp4TrackInfo& track) {
  uint8_t b[32];
  bool haveTables = false;
  for (uint64_t pos = start; pos + 8 <= end;) {
    Box box;
    if (!readBox(*cache_, pos, end, box)) return false;
    if (box.type == fourcc("tkhd")) {
      if (!cache_->readAt(box.body, b, 1)) return false;
      const uint64_t idPos = box.body + (b[0] == 1 ? 20 : 12);
      if (!cache_->readAt(idPos, b, 4)) return false;
      track.trackId = be32(b);
    } else if (box.type == fourcc("tref")) {
      Box chap;
      for (uint64_t p = box.body; p + 8 <= box.end; p = chap.end) {
        if (!readBox(*cache_, p, box.end, chap)) break;
        if (chap.type == fourcc("chap") && chap.end - chap.body >= 4 && cache_->readAt(chap.body, b, 4)) {
          track.chapterTrackId = be32(b);
        }
      }
    } else if (box.type == fourcc("mdia")) {
      for (uint64_t p = box.body; p + 8 <= box.end;) {
        Box child;
        if (!readBox(*cache_, p, box.end, child)) return false;
        if (child.type == fourcc("mdhd")) {
          if (!cache_->readAt(child.body, b, 1)) return false;
          if (b[0] == 1) {
            if (!cache_->readAt(child.body + 20, b, 12)) return false;
            track.timescale = be32(b);
            track.duration = be64(b + 4);
          } else {
            if (!cache_->readAt(child.body + 12, b, 8)) return false;
            track.timescale = be32(b);
            track.duration = be32(b + 4);
          }
        } else if (child.type == fourcc("hdlr")) {
          if (!cache_->readAt(child.body + 8, b, 4)) return false;
          track.handler = be32(b);
        } else if (child.type == fourcc("minf")) {
          for (uint64_t q = child.body; q + 8 <= child.end;) {
            Box stbl;
            if (!readBox(*cache_, q, child.end, stbl)) return false;
            if (stbl.type == fourcc("stbl")) haveTables = parseStbl(stbl.body, stbl.end, track);
            q = stbl.end;
          }
        }
        p = child.end;
      }
    }
    pos = box.end;
  }
  return haveTables;
}

bool Mp4Reader::parseStbl(const uint64_t start, const uint64_t end, Mp4TrackInfo& track) {
  uint8_t b[12];
  bool stts = false, stsc = false, stsz = false, stco = false;
  // Each table: version/flags, entry count, then fixed-size entries that must
  // fit inside the box.
  auto table = [&](const Box& box, const uint8_t entrySize, uint64_t& offset, uint32_t& count) {
    if (!cache_->readAt(box.body, b, 8)) return false;
    count = be32(b + 4);
    offset = box.body + 8;
    return static_cast<uint64_t>(count) * entrySize <= box.end - offset;
  };
  for (uint64_t pos = start; pos + 8 <= end;) {
    Box box;
    if (!readBox(*cache_, pos, end, box)) return false;
    if (box.type == fourcc("stsd")) {
      parseStsd(box.body, box.end, track);
    } else if (box.type == fourcc("stts")) {
      stts = table(box, 8, track.sttsOffset, track.sttsCount);
    } else if (box.type == fourcc("stsc")) {
      stsc = table(box, 12, track.stscOffset, track.stscCount);
    } else if (box.type == fourcc("stco")) {
      stco = table(box, 4, track.chunkOffsetsOffset, track.chunkCount);
      track.chunkOffsets64 = false;
    } else if (box.type == fourcc("co64")) {
      stco = table(box, 8, track.chunkOffsetsOffset, track.chunkCount);
      track.chunkOffsets64 = true;
    } else if (box.type == fourcc("stsz")) {
      if (!cache_->readAt(box.body, b, 12)) return false;
      track.constantSampleSize = be32(b + 4);
      track.sampleCount = be32(b + 8);
      track.stszOffset = box.body + 12;
      stsz =
          track.constantSampleSize != 0 || static_cast<uint64_t>(track.sampleCount) * 4 <= box.end - track.stszOffset;
    }
    pos = box.end;
  }
  return stts && stsc && stsz && stco && track.stscCount > 0 && track.chunkCount > 0;
}

bool Mp4Reader::parseStsd(const uint64_t start, const uint64_t end, Mp4TrackInfo& track) {
  // version/flags, entry count, then the first sample entry box.
  Box entry;
  if (!readBox(*cache_, start + 8, end, entry)) return false;
  const bool aac = entry.type == fourcc("mp4a");
  const bool mp3 = entry.type == fourcc(".mp3");
  if (!aac && !mp3) return false;

  // Audio sample entry: 8 bytes SampleEntry, then version, revision, vendor,
  // channels, sample size, compression id, packet size, 16.16 rate.
  uint8_t b[28];
  if (entry.end - entry.body < sizeof(b) || !cache_->readAt(entry.body, b, sizeof(b))) return false;
  const uint16_t version = be16(b + 8);
  track.entryChannels = be16(b + 16);
  track.entrySampleRate = be32(b + 24) >> 16;
  uint64_t children = entry.body + sizeof(b);
  if (version == 1) children += 16;
  if (version == 2) children += 36;

  if (mp3) {
    track.isMp3 = true;
    return true;
  }
  return parseSampleEntryChildren(children, entry.end, track, 0);
}

bool Mp4Reader::parseSampleEntryChildren(const uint64_t start, const uint64_t end, Mp4TrackInfo& track,
                                         const int depth) {
  if (depth > MAX_BOX_DEPTH) return false;
  for (uint64_t pos = start; pos + 8 <= end;) {
    Box box;
    if (!readBox(*cache_, pos, end, box)) return false;
    if (box.type == fourcc("esds")) return parseEsds(box.body, box.end, track);
    // QuickTime files wrap esds in a 'wave' box.
    if (box.type == fourcc("wave") && parseSampleEntryChildren(box.body, box.end, track, depth + 1)) return true;
    pos = box.end;
  }
  return false;
}

bool Mp4Reader::parseEsds(const uint64_t start, const uint64_t end, Mp4TrackInfo& track) {
  uint8_t d[128];
  const uint64_t span = end - start;
  if (span < 4) return false;
  const size_t avail = static_cast<size_t>(span - 4 < sizeof(d) ? span - 4 : sizeof(d));
  if (!cache_->readAt(start + 4, d, avail)) return false;  // skip version/flags

  size_t pos = 0;
  uint32_t length = 0;
  if (pos >= avail || d[pos++] != 0x03 || !readDescriptorLength(d, avail, pos, length)) return false;
  if (pos + 3 > avail) return false;
  const uint8_t flags = d[pos + 2];
  pos += 3;  // ES_ID, flags
  if (flags & 0x80) pos += 2;
  if (flags & 0x40) {
    if (pos >= avail) return false;
    pos += 1 + d[pos];
  }
  if (flags & 0x20) pos += 2;

  if (pos >= avail || d[pos++] != 0x04 || !readDescriptorLength(d, avail, pos, length)) return false;
  if (pos + 13 > avail) return false;
  const uint8_t objectType = d[pos];
  pos += 13;
  if (objectType == OTI_MPEG1_AUDIO || objectType == OTI_MPEG2_AUDIO) {
    track.isMp3 = true;
    return true;
  }

  if (pos >= avail || d[pos++] != 0x05 || !readDescriptorLength(d, avail, pos, length)) return false;
  if (pos + length > avail || !parseAacConfig(d + pos, length, track.aac)) return false;
  if (track.aac.channels == 0) track.aac.channels = static_cast<uint8_t>(track.entryChannels);
  track.isAac = track.aac.channels > 0 && track.aac.channels <= 2;
  return track.isAac;
}

// ---------------------------------------------------------------------------
// Chapters

bool Mp4Reader::allocateChapters(size_t count) {
  if (count > MAX_CHAPTERS) count = MAX_CHAPTERS;
  chapters_ = makeUniqueNoThrow<Chapter[]>(count);
  chapterCount_ = 0;
  return chapters_ != nullptr;
}

void Mp4Reader::parseChpl(const uint64_t start, const uint64_t end) {
  // Nero chapters: version, flags, (version 1: 4 reserved bytes), count, then
  // per chapter a 64-bit start in 100 ns units and a length-prefixed title.
  uint8_t b[9];
  if (!cache_->readAt(start, b, 4)) return;
  uint64_t pos = start + 4 + (b[0] == 1 ? 4 : 0);
  if (pos >= end || !cache_->readAt(pos++, b, 1)) return;
  const size_t count = b[0];
  if (count == 0 || !allocateChapters(count)) return;

  char title[256];
  for (size_t i = 0; i < count && chapterCount_ < MAX_CHAPTERS; i++) {
    if (pos + 9 > end || !cache_->readAt(pos, b, 9)) break;
    const uint64_t start100ns = be64(b);
    const size_t len = b[8];
    pos += 9;
    if (pos + len > end || !cache_->readAt(pos, reinterpret_cast<uint8_t*>(title), len)) break;
    pos += len;
    Chapter& c = chapters_[chapterCount_++];
    c.startMs = start100ns / 10000;
    copyTitle(title, len, c.title, sizeof(c.title));
  }
}

void Mp4Reader::loadTextChapters(const Mp4TrackInfo& textTrack) {
  if (textTrack.timescale == 0 || textTrack.sampleCount == 0) return;
  Mp4SampleIterator it;
  if (!it.init(source_, textTrack) || !allocateChapters(textTrack.sampleCount)) return;

  uint8_t data[MAX_CHAPTER_SAMPLE_BYTES];
  while (!it.atEnd() && chapterCount_ < MAX_CHAPTERS) {
    uint32_t size = 0;
    if (!it.size(size)) break;
    const size_t want = size < sizeof(data) ? size : sizeof(data);
    Chapter& c = chapters_[chapterCount_];
    c.startMs = it.time() * 1000 / textTrack.timescale;
    c.title[0] = '\0';
    if (want >= 2 && cache_->readAt(it.offset(), data, want)) {
      // A 16-bit length, then the text. UTF-16 titles (with a BOM) are rare
      // in audiobooks and are left untitled.
      size_t len = be16(data);
      if (len > want - 2) len = want - 2;
      const bool utf16 = len >= 2 && ((data[2] == 0xFE && data[3] == 0xFF) || (data[2] == 0xFF && data[3] == 0xFE));
      if (!utf16) copyTitle(reinterpret_cast<const char*>(data + 2), len, c.title, sizeof(c.title));
    }
    chapterCount_++;
    if (!it.advance()) break;
  }
}

// ---------------------------------------------------------------------------
// Playback

uint32_t Mp4Reader::sampleRate() const {
  if (audio_.isAac) return audio_.aac.coreRate;
  return audio_.entrySampleRate;
}

uint8_t Mp4Reader::channels() const {
  if (audio_.isAac) return audio_.aac.channels;
  return static_cast<uint8_t>(audio_.entryChannels);
}

uint64_t Mp4Reader::durationMs() const {
  if (audio_.timescale == 0) return 0;
  return audio_.duration * 1000 / audio_.timescale;
}

bool Mp4Reader::seekMs(const uint64_t ms, uint64_t& actualMs) {
  if (!haveAudio_) return false;
  if (!samples_.seekTime(ms * audio_.timescale / 1000)) return false;
  actualMs = samples_.atEnd() ? durationMs() : samples_.time() * 1000 / audio_.timescale;
  return true;
}

int Mp4Reader::nextPacket(uint8_t* dst, const size_t capacity, uint64_t& startMs) {
  if (!haveAudio_ || samples_.atEnd()) return 0;
  uint32_t size = 0;
  if (!samples_.size(size)) return -1;
  if (size > capacity) return -1;
  if (!cache_->readAt(samples_.offset(), dst, size)) return -1;
  startMs = samples_.time() * 1000 / audio_.timescale;
  if (!samples_.advance() && !samples_.atEnd()) return -1;
  return static_cast<int>(size);
}

}  // namespace audiobook
