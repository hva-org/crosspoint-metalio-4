#pragma once

// Pure helpers for the audiobook player: which files are books, how tracks of
// a folder are ordered, where chapters start, and the saved-position format.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace audiobook {

// .mp3, .m4b and .m4a, case-insensitive.
bool isAudiobookFile(std::string_view name);
// .m4b and .m4a: one file with its own chapters.
bool isMp4Book(std::string_view name);

// What a book is saved under: an MP3 inside a folder belongs to a book made
// of that folder's MP3s, so the key is the folder. An M4B/M4A, or an MP3 at
// the card root, is a book of its own and the key is the file.
std::string progressKey(std::string_view path);
// The cache folder that holds the book's saved position.
std::string progressDir(std::string_view path);
// The same for a folder book, from its folder (trailing slashes ignored).
std::string folderProgressDir(std::string_view folder);
// True when changing `changed` (a file or folder) changes `target`: they are
// the same path, or `changed` is a folder that contains `target`.
bool affectsPath(std::string_view changed, std::string_view target);

// Orders names case-insensitively, comparing digit runs by value, so
// "Part 2" sorts before "Part 10". Returns <0, 0 or >0 like strcmp.
int naturalCompare(std::string_view a, std::string_view b);

// One place in the book: a chapter starts at `startMs` within `track`.
struct ChapterMark {
  uint16_t track = 0;
  uint64_t startMs = 0;
};

// Index of the last chapter that starts at or before (track, ms). Chapters
// must be sorted by (track, startMs). Returns 0 for an empty list.
size_t chapterAt(const ChapterMark* chapters, size_t count, uint16_t track, uint64_t ms);

// Where a slider at `permille` (0-1000) lands in a chapter `lengthMs` long,
// from the chapter start. Stops 1 s before the end so a seek stays in the
// chapter; 0 when the length is unknown.
uint64_t chapterSeekOffsetMs(uint64_t lengthMs, int32_t permille);

// Whole minutes for the chapter time labels. Elapsed rounds down and the
// length up, and the time left is their difference, so both labels change
// once a minute, together, and add up to the length. The time left can read
// up to a minute high, never low. `left` is 0 while the length is unknown (0).
struct ChapterMinutes {
  uint32_t elapsed = 0;
  uint32_t length = 0;
  uint32_t left = 0;
};
ChapterMinutes chapterMinutes(uint64_t elapsedMs, uint64_t lengthMs);

struct SavedPosition {
  uint16_t track = 0;
  uint32_t positionMs = 0;
};

inline constexpr size_t PROGRESS_BYTES = 7;
// Version byte, then little-endian track and position.
void encodeProgress(const SavedPosition& position, uint8_t (&out)[PROGRESS_BYTES]);
bool decodeProgress(const uint8_t* data, size_t len, SavedPosition& out);

}  // namespace audiobook
