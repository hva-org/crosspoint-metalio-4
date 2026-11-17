#include "TrackReader.h"

#include <cstring>

namespace audiobook {

void copyTitle(const char* src, size_t srcLen, char* dst, const size_t capacity) {
  if (capacity == 0) return;
  const void* nul = std::memchr(src, '\0', srcLen);
  if (nul) srcLen = static_cast<size_t>(static_cast<const char*>(nul) - src);
  size_t n = srcLen;
  if (n > capacity - 1) {
    n = capacity - 1;
    // Back up over continuation bytes so a multi-byte character is not cut.
    while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80) n--;
  }
  while (n > 0 && static_cast<unsigned char>(src[n - 1]) <= ' ') n--;
  std::memcpy(dst, src, n);
  dst[n] = '\0';
}

}  // namespace audiobook
