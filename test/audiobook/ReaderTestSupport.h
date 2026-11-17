#pragma once

#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "ByteSource.h"

// In-memory ByteSource for reader tests. Optionally returns short reads to
// exercise callers' loops.
class MemorySource final : public audiobook::ByteSource {
 public:
  explicit MemorySource(std::vector<uint8_t> data, size_t maxRead = SIZE_MAX)
      : data_(std::move(data)), maxRead_(maxRead) {}

  int readAt(uint64_t pos, uint8_t* dst, size_t len) override {
    reads++;
    if (pos >= data_.size()) return 0;
    size_t n = std::min(len, static_cast<size_t>(data_.size() - pos));
    n = std::min(n, maxRead_);
    std::memcpy(dst, data_.data() + pos, n);
    return static_cast<int>(n);
  }
  uint64_t size() const override { return data_.size(); }

  std::vector<uint8_t>& data() { return data_; }
  size_t reads = 0;

 private:
  std::vector<uint8_t> data_;
  size_t maxRead_;
};

inline std::vector<uint8_t> loadFixture(const std::string& name) {
  std::ifstream in(std::string(AUDIOBOOK_FIXTURE_DIR) + "/" + name, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Replaces the first occurrence of a 4-byte box type, e.g. to hide 'chpl'.
inline bool renameBox(std::vector<uint8_t>& data, const char* from, const char* to) {
  for (size_t i = 4; i + 4 <= data.size(); i++) {
    if (std::memcmp(data.data() + i, from, 4) == 0) {
      std::memcpy(data.data() + i, to, 4);
      return true;
    }
  }
  return false;
}
