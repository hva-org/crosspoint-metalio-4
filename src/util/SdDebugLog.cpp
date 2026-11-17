#include "SdDebugLog.h"

#if CROSSPOINT_SD_LOG

#include <HalMemory.h>
#include <HalStorage.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>

#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"

namespace sdlog {
namespace {

constexpr char LOG_PATH[] = "/.crosspoint/debug.log";
constexpr char OLD_LOG_PATH[] = "/.crosspoint/debug.old.log";
// PSRAM; holds several seconds of busy debug output between writes.
constexpr size_t RING_BYTES = 32 * 1024;
// Internal RAM; the unit of each card write.
constexpr size_t CHUNK_BYTES = 2048;
constexpr uint32_t FLUSH_MS = 1000;
constexpr uint64_t MAX_FILE_BYTES = 2ull * 1024 * 1024;

// Guards the ring against loggers on both cores; held only to copy bytes.
portMUX_TYPE ringLock = portMUX_INITIALIZER_UNLOCKED;
HalMemory::PsramBuffer ring;  // null while off
size_t head = 0;              // next write
size_t tail = 0;              // next read; head == tail means empty
uint32_t dropped = 0;         // lines that did not fit

bool active = false;
bool allocationFailed = false;
uint32_t lastFlushMs = 0;
char chunk[CHUNK_BYTES];

size_t usedLocked() { return (head + RING_BYTES - tail) % RING_BYTES; }

// The log sink: runs inside logPrintf() on any task. Copies only.
void capture(const char* line) {
  const size_t len = strlen(line);
  portENTER_CRITICAL_SAFE(&ringLock);
  if (ring) {
    if (len >= RING_BYTES - 1 - usedLocked()) {
      dropped++;
    } else {
      const size_t first = len < RING_BYTES - head ? len : RING_BYTES - head;
      memcpy(ring.get() + head, line, first);
      memcpy(ring.get(), line + first, len - first);
      head = (head + len) % RING_BYTES;
    }
  }
  portEXIT_CRITICAL_SAFE(&ringLock);
}

// Moves up to one chunk out of the ring.
size_t take(uint32_t& droppedLines) {
  portENTER_CRITICAL_SAFE(&ringLock);
  size_t n = ring ? usedLocked() : 0;
  if (n > CHUNK_BYTES) n = CHUNK_BYTES;
  const size_t first = n < RING_BYTES - tail ? n : RING_BYTES - tail;
  if (n > 0) {
    memcpy(chunk, ring.get() + tail, first);
    memcpy(chunk + first, ring.get(), n - first);
    tail = (tail + n) % RING_BYTES;
  }
  droppedLines = dropped;
  dropped = 0;
  portEXIT_CRITICAL_SAFE(&ringLock);
  return n;
}

HalFile openLog() {
  HalFile file = Storage.open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND);
  if (file && file.isOpen() && file.fileSize64() > MAX_FILE_BYTES) {
    file.close();
    Storage.remove(OLD_LOG_PATH);
    Storage.rename(LOG_PATH, OLD_LOG_PATH);
    file = Storage.open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND);
  }
  return file;
}

void writePending() {
  uint32_t droppedLines = 0;
  size_t n = take(droppedLines);
  if (n == 0 && droppedLines == 0) return;
  if (!Storage.ensureDirectoryExists("/.crosspoint")) return;
  HalFile file = openLog();
  if (!file || !file.isOpen()) return;  // the lines taken are lost; the next write tries again
  // Bounded to one ring's worth, so lines logged meanwhile cannot keep it going.
  // Lines are dropped only when the ring is full, so the gap comes after what
  // the ring held: the note follows those lines.
  uint32_t droppedTotal = droppedLines;
  size_t total = 0;
  while (n > 0) {
    if (file.write(reinterpret_cast<const uint8_t*>(chunk), n) != n) break;
    total += n;
    if (total >= RING_BYTES) break;  // the rest waits for the next write
    n = take(droppedLines);
    droppedTotal += droppedLines;
  }
  if (droppedTotal > 0) {
    char note[48];
    const int len = snprintf(note, sizeof(note), "[sdlog] %u lines dropped\n", static_cast<unsigned>(droppedTotal));
    if (len > 0) file.write(reinterpret_cast<const uint8_t*>(note), static_cast<size_t>(len));
  }
  file.close();
}

void start() {
  if (allocationFailed) return;
  HalMemory::PsramBuffer buffer = HalMemory::allocatePsram(RING_BYTES);
  if (!buffer) {
    allocationFailed = true;  // do not retry every loop
    LOG_ERR("SDLOG", "No PSRAM for the debug log");
    return;
  }
  portENTER_CRITICAL(&ringLock);
  ring = std::move(buffer);
  head = tail = 0;
  dropped = 0;
  portEXIT_CRITICAL(&ringLock);
  setLogSink(capture);
  active = true;
  LOG_INF("SDLOG", "Debug log on, %s", LOG_PATH);
}

void stop() {
  LOG_INF("SDLOG", "Debug log off");
  setLogSink(nullptr);
  writePending();
  HalMemory::PsramBuffer old;
  portENTER_CRITICAL(&ringLock);
  old = std::move(ring);
  portEXIT_CRITICAL(&ringLock);
  active = false;
}

}  // namespace

void tick(const bool storageFree) {
  const bool wanted = SETTINGS.debugLogToSd != 0;
  if (wanted && !active) {
    start();
  } else if (!wanted && active) {
    stop();
  }
  if (!active || !storageFree) return;
  const uint32_t now = millis();
  size_t pending = 0;
  portENTER_CRITICAL(&ringLock);
  pending = usedLocked();
  portEXIT_CRITICAL(&ringLock);
  if (now - lastFlushMs < FLUSH_MS && pending < RING_BYTES / 2) return;
  lastFlushMs = now;
  writePending();
}

void flush() {
  if (active) writePending();
}

}  // namespace sdlog

#endif
