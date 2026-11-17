#pragma once

// Random-access byte input for the audiobook readers. Device code wraps a
// HalFile; host tests use memory. Every read is positioned, so one source can
// serve table lookups and packet reads without a shared cursor.

#include <cstddef>
#include <cstdint>
#include <memory>

namespace audiobook {

class ByteSource {
 public:
  virtual ~ByteSource() = default;
  // Reads up to `len` bytes at `pos`. Returns the count, 0 at end, <0 on error.
  virtual int readAt(uint64_t pos, uint8_t* dst, size_t len) = 0;
  virtual uint64_t size() const = 0;
};

// Reads exactly `len` bytes at `pos`, looping over short reads.
bool readExact(ByteSource& src, uint64_t pos, uint8_t* dst, size_t len);

inline uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
inline uint32_t be32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 |
         p[3];
}
inline uint64_t be64(const uint8_t* p) { return static_cast<uint64_t>(be32(p)) << 32 | be32(p + 4); }

// One read-ahead window over a ByteSource, sized at init. Sequential frames
// and samples that sit next to each other are served from it.
class CachedReader {
 public:
  // Allocates the window. Returns false on allocation failure.
  bool init(ByteSource* source, size_t capacity);
  void invalidate() { length_ = 0; }

  // Copies exactly `len` bytes at `pos`.
  bool readAt(uint64_t pos, uint8_t* dst, size_t len);
  // Makes up to `len` bytes at `pos` (len <= capacity) readable in place.
  // Returns the number available; fewer than `len` only near the end.
  size_t peekAt(uint64_t pos, size_t len, const uint8_t** out);

  uint64_t size() const { return source_ ? source_->size() : 0; }
  size_t capacity() const { return capacity_; }

 private:
  bool fill(uint64_t pos);

  ByteSource* source_ = nullptr;
  std::unique_ptr<uint8_t[]> buffer_;
  size_t capacity_ = 0;
  uint64_t start_ = 0;
  size_t length_ = 0;
};

}  // namespace audiobook
