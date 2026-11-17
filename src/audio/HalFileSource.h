#pragma once

#if CROSSPOINT_AUDIOBOOK

#include <ByteSource.h>
#include <HalStorage.h>

namespace audiobook {

// ByteSource over one SD file. Each read takes the storage mutex for that
// call only, so the UI can use the card between audio reads.
class HalFileSource final : public ByteSource {
 public:
  bool open(const char* path) {
    if (!Storage.openFileForRead("ABK", path, file_)) return false;
    size_ = file_.fileSize64();
    return true;
  }
  void close() {
    if (file_) file_.close();
    size_ = 0;
  }
  int readAt(const uint64_t pos, uint8_t* dst, const size_t len) override {
    if (!file_.seek64(pos)) return -1;
    return file_.read(dst, len);
  }
  uint64_t size() const override { return size_; }

 private:
  HalFile file_;
  uint64_t size_ = 0;
};

}  // namespace audiobook

#endif
