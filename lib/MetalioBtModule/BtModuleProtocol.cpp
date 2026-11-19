#include "BtModuleProtocol.h"

#include <cctype>
#include <cstring>

namespace metalio_bt {
namespace {
constexpr char FOUND_PREFIX[] = "AT+BT:";
constexpr char MODE_PREFIX[] = "SET MODE ";

bool isHex(const char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }

// Copies at most capacity-1 bytes without splitting a UTF-8 sequence, then
// drops trailing spaces.
void copyName(const char* src, char* dst, const size_t capacity) {
  size_t n = std::strlen(src);
  if (n > capacity - 1) {
    n = capacity - 1;
    // Back up over continuation bytes so a multi-byte character is not cut.
    while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80) n--;
  }
  while (n > 0 && std::isspace(static_cast<unsigned char>(src[n - 1]))) n--;
  std::memcpy(dst, src, n);
  dst[n] = '\0';
}
}  // namespace

bool LineReader::feed(const char c) {
  if (pendingReset_) {
    length_ = 0;
    pendingReset_ = false;
  }
  // Either CR or LF ends a line, so CR-only replies parse too; the LF of a
  // CRLF pair then ends an empty line, which is skipped.
  if (c == '\r' || c == '\n') {
    const bool complete = !overflowed_ && length_ > 0;
    overflowed_ = false;
    buffer_[length_] = '\0';
    pendingReset_ = true;
    return complete;
  }
  if (overflowed_) return false;
  if (length_ + 1 >= CAPACITY) {
    overflowed_ = true;
    return false;
  }
  buffer_[length_++] = c;
  return false;
}

bool LineReader::flush() {
  if (pendingReset_) return false;  // the last line was already handed over
  const bool complete = !overflowed_ && length_ > 0;
  overflowed_ = false;
  buffer_[length_] = '\0';
  pendingReset_ = true;
  return complete;
}

Event parseLine(const char* line) {
  Event event;
  if (line == nullptr) return event;

  if (std::strncmp(line, FOUND_PREFIX, sizeof(FOUND_PREFIX) - 1) == 0) {
    const char* payload = line + sizeof(FOUND_PREFIX) - 1;
    for (size_t i = 0; i < ADDRESS_LENGTH; i++) {
      if (!isHex(payload[i])) return event;
      event.address[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(payload[i])));
    }
    copyName(payload + ADDRESS_LENGTH, event.name, NAME_CAPACITY);
    event.type = EventType::DeviceFound;
    return event;
  }

  // RECONNECT and CONNECTING both contain "CONNECT"; a success line wins,
  // including a "RECONNECT SUCCESS".
  if (const char* mode = std::strstr(line, MODE_PREFIX)) {
    const char digit = mode[sizeof(MODE_PREFIX) - 1];
    if (digit >= '1' && digit <= '3') {
      event.type = EventType::ModeSet;
      event.mode = static_cast<uint8_t>(digit - '0');
    }
  } else if (std::strstr(line, "SLC CONNECT SUCCESS")) {
    // The hands-free service link that follows the audio link's
    // "CONNECT SUCCESS". Taking it as a second connect sent a second AT+PP=1,
    // which paused the stream (seen on the device).
  } else if (std::strstr(line, "CONNECT SUCCESS")) {
    event.type = EventType::Connected;
  } else if (std::strstr(line, "REMOTE DISCON")) {
    event.type = EventType::Disconnected;
  } else if (std::strstr(line, "RECONNECT")) {
    event.type = EventType::Reconnect;
  } else if (std::strstr(line, "INQUIRING START")) {
    event.type = EventType::ScanStarted;
  } else if (std::strstr(line, "INQ COMPLETE")) {
    event.type = EventType::ScanDone;
  } else if (std::strstr(line, "CONNECTING")) {
    event.type = EventType::Connecting;
  } else if (std::strstr(line, "CONNECT TIMEOUT")) {
    event.type = EventType::ConnectTimeout;
  }
  return event;
}

bool isValidAddress(const char* address) {
  if (address == nullptr) return false;
  for (size_t i = 0; i < ADDRESS_LENGTH; i++) {
    if (!isHex(address[i])) return false;
  }
  return address[ADDRESS_LENGTH] == '\0';
}

size_t formatConnect(const char* address, char* out, const size_t capacity) {
  constexpr char PREFIX[] = "AT+CONNECT=";
  constexpr size_t LENGTH = sizeof(PREFIX) - 1 + ADDRESS_LENGTH + 2;
  if (!isValidAddress(address) || out == nullptr || capacity < LENGTH + 1) return 0;
  std::memcpy(out, PREFIX, sizeof(PREFIX) - 1);
  std::memcpy(out + sizeof(PREFIX) - 1, address, ADDRESS_LENGTH);
  std::memcpy(out + sizeof(PREFIX) - 1 + ADDRESS_LENGTH, "\r\n", 3);
  return LENGTH;
}

}  // namespace metalio_bt
