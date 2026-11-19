#include "ByteSource.h"

#include <Memory.h>

#include <cstring>

namespace audiobook {

bool readExact(ByteSource& src, uint64_t pos, uint8_t* dst, size_t len) {
  while (len > 0) {
    const int n = src.readAt(pos, dst, len);
    if (n <= 0) return false;
    pos += static_cast<uint64_t>(n);
    dst += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

bool CachedReader::init(ByteSource* source, const size_t capacity) {
  source_ = source;
  length_ = 0;
  if (capacity_ != capacity || !buffer_) {
    buffer_ = makeUniqueNoThrow<uint8_t[]>(capacity);
    capacity_ = buffer_ ? capacity : 0;
  }
  return buffer_ != nullptr;
}

bool CachedReader::fill(const uint64_t pos) {
  length_ = 0;
  start_ = pos;
  const uint64_t total = source_->size();
  if (pos >= total) return false;
  const size_t want = static_cast<size_t>(total - pos < capacity_ ? total - pos : capacity_);
  while (length_ < want) {
    const int n = source_->readAt(pos + length_, buffer_.get() + length_, want - length_);
    if (n <= 0) break;
    length_ += static_cast<size_t>(n);
  }
  return length_ > 0;
}

size_t CachedReader::peekAt(const uint64_t pos, size_t len, const uint8_t** out) {
  if (!source_ || !buffer_) return 0;
  if (len > capacity_) len = capacity_;
  const bool hit = pos >= start_ && pos + len <= start_ + length_;
  if (!hit && !fill(pos)) return 0;
  const size_t offset = static_cast<size_t>(pos - start_);
  *out = buffer_.get() + offset;
  const size_t available = length_ - offset;
  return available < len ? available : len;
}

bool CachedReader::readAt(const uint64_t pos, uint8_t* dst, const size_t len) {
  if (!source_ || !buffer_) return false;
  if (len > capacity_) return readExact(*source_, pos, dst, len);
  const uint8_t* p = nullptr;
  if (peekAt(pos, len, &p) != len) return false;
  std::memcpy(dst, p, len);
  return true;
}

}  // namespace audiobook
