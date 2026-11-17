#include "BookModel.h"

#include <functional>

namespace audiobook {
namespace {
constexpr uint8_t PROGRESS_VERSION = 1;

char lower(const char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
bool isDigit(const char c) { return c >= '0' && c <= '9'; }

bool endsWithNoCase(const std::string_view name, const std::string_view suffix) {
  if (name.size() < suffix.size()) return false;
  const std::string_view tail = name.substr(name.size() - suffix.size());
  for (size_t i = 0; i < suffix.size(); i++) {
    if (lower(tail[i]) != suffix[i]) return false;
  }
  return true;
}
}  // namespace

bool isMp4Book(const std::string_view name) { return endsWithNoCase(name, ".m4b") || endsWithNoCase(name, ".m4a"); }

bool isAudiobookFile(const std::string_view name) { return endsWithNoCase(name, ".mp3") || isMp4Book(name); }

std::string progressKey(const std::string_view path) {
  const size_t slash = path.find_last_of('/');
  const bool inFolder = slash != std::string_view::npos && slash > 0;
  if (inFolder && !isMp4Book(path)) return std::string(path.substr(0, slash));
  return std::string(path);
}

namespace {
std::string progressDirForKey(const std::string& key) {
  return "/.crosspoint/audio_" + std::to_string(std::hash<std::string>{}(key));
}
}  // namespace

std::string progressDir(const std::string_view path) { return progressDirForKey(progressKey(path)); }

std::string folderProgressDir(std::string_view folder) {
  while (folder.size() > 1 && folder.back() == '/') folder.remove_suffix(1);
  return progressDirForKey(std::string(folder));
}

bool affectsPath(std::string_view changed, const std::string_view target) {
  while (changed.size() > 1 && changed.back() == '/') changed.remove_suffix(1);
  if (changed == target) return true;
  if (changed == "/") return true;
  return target.size() > changed.size() && target.substr(0, changed.size()) == changed && target[changed.size()] == '/';
}

int naturalCompare(const std::string_view a, const std::string_view b) {
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    if (isDigit(a[i]) && isDigit(b[j])) {
      // Skip leading zeros, then the longer digit run is the larger number.
      while (i < a.size() && a[i] == '0') i++;
      while (j < b.size() && b[j] == '0') j++;
      size_t endA = i, endB = j;
      while (endA < a.size() && isDigit(a[endA])) endA++;
      while (endB < b.size() && isDigit(b[endB])) endB++;
      const size_t lenA = endA - i, lenB = endB - j;
      if (lenA != lenB) return lenA < lenB ? -1 : 1;
      for (; i < endA; i++, j++) {
        if (a[i] != b[j]) return a[i] < b[j] ? -1 : 1;
      }
      continue;
    }
    const char ca = lower(a[i]), cb = lower(b[j]);
    if (ca != cb) return ca < cb ? -1 : 1;
    i++;
    j++;
  }
  if (i < a.size()) return 1;
  if (j < b.size()) return -1;
  return 0;
}

size_t chapterAt(const ChapterMark* chapters, const size_t count, const uint16_t track, const uint64_t ms) {
  size_t found = 0;
  for (size_t i = 0; i < count; i++) {
    const bool before = chapters[i].track < track || (chapters[i].track == track && chapters[i].startMs <= ms);
    if (!before) break;
    found = i;
  }
  return found;
}

void encodeProgress(const SavedPosition& position, uint8_t (&out)[PROGRESS_BYTES]) {
  out[0] = PROGRESS_VERSION;
  out[1] = static_cast<uint8_t>(position.track);
  out[2] = static_cast<uint8_t>(position.track >> 8);
  for (int i = 0; i < 4; i++) out[3 + i] = static_cast<uint8_t>(position.positionMs >> (8 * i));
}

bool decodeProgress(const uint8_t* data, const size_t len, SavedPosition& out) {
  if (data == nullptr || len < PROGRESS_BYTES || data[0] != PROGRESS_VERSION) return false;
  out.track = static_cast<uint16_t>(data[1] | data[2] << 8);
  out.positionMs = 0;
  for (int i = 0; i < 4; i++) out.positionMs |= static_cast<uint32_t>(data[3 + i]) << (8 * i);
  return true;
}

}  // namespace audiobook
