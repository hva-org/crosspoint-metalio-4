#pragma once

// Splits one audio file into compressed packets with start times. The reader
// task copies packets into the ring; the decode task never touches the file.

#include <cstddef>
#include <cstdint>

namespace audiobook {

enum class Codec : uint8_t { Mp3, Aac };

// MP3 frames are at most 1441 bytes; raw stereo AAC frames at most 1536.
inline constexpr size_t MAX_PACKET_BYTES = 4096;

inline constexpr size_t CHAPTER_TITLE_CAPACITY = 48;  // including the terminator

struct Chapter {
  uint64_t startMs = 0;
  char title[CHAPTER_TITLE_CAPACITY] = {};
};

class TrackReader {
 public:
  virtual ~TrackReader() = default;

  virtual Codec codec() const = 0;
  // Stored rate; HE-AAC decodes to twice this.
  virtual uint32_t sampleRate() const = 0;
  virtual uint8_t channels() const = 0;
  virtual uint64_t durationMs() const = 0;

  // Moves to the packet that starts at or before `ms`. `actualMs` receives
  // that packet's start time. Seeking past the end positions at end of file.
  virtual bool seekMs(uint64_t ms, uint64_t& actualMs) = 0;

  // Copies the next packet into `dst`. Returns its size, 0 at end of track,
  // or <0 on a read error or a packet larger than `capacity`.
  virtual int nextPacket(uint8_t* dst, size_t capacity, uint64_t& startMs) = 0;
};

// Copies `src` into a title buffer without splitting a UTF-8 sequence and
// without trailing whitespace or control bytes.
void copyTitle(const char* src, size_t srcLen, char* dst, size_t capacity);

}  // namespace audiobook
