#pragma once

// Text protocol of the Metalio E-Ink 4 Bluetooth audio module (UART2, 115200).
// Commands and replies follow the vendor's MetalioClaw4 firmware
// (main/display/screen/bluetooth_screen/bluetooth_screen.cc); the E-Ink 4
// SDK sends the same mode-1 sequence. Pure code: no Arduino or SDK types.

#include <cstddef>
#include <cstdint>

namespace metalio_bt {

// Mode 1: the module plays ESP32 I2S on the board speaker (SDK default).
inline constexpr char CMD_SPEAKER_ROLE[] = "AT+RX=2\r\n";
inline constexpr char CMD_SPEAKER_MODE[] = "AT+MODE=1\r\n";
// Mode 2: the module sends ESP32 I2S to a Bluetooth headset.
inline constexpr char CMD_HEADSET_ROLE[] = "AT+TX=1\r\n";
inline constexpr char CMD_HEADSET_MODE[] = "AT+MODE=2\r\n";
inline constexpr char CMD_SCAN[] = "AT+INQUIRING\r\n";
// Music (A2DP) audio to the connected headset, sent in this order.
inline constexpr char CMD_MUSIC_PROFILE[] = "AT+BTSCO=0\r\n";
inline constexpr char CMD_MUSIC_START[] = "AT+PP=1\r\n";

// Delays between commands, from a firmware that drives this module on the
// E-Ink 4: role to mode, after headset mode, and between the music commands.
inline constexpr uint32_t ROLE_TO_MODE_DELAY_MS = 700;
inline constexpr uint32_t HEADSET_MODE_SETTLE_MS = 300;
inline constexpr uint32_t MUSIC_STEP_DELAY_MS = 20;
// After AT+MODE=1, before the clock is checked. Our choice: no source gives
// how long the module takes to start clocking.
inline constexpr uint32_t SPEAKER_MODE_SETTLE_MS = 500;
// Least time from one mode command to the next switch's first command. The
// reference never sends them closer than a user action; back to back, the
// module ignored the second switch.
inline constexpr uint32_t MODE_GAP_MS = 2000;
// Before a scan: the longest wait for "SET MODE 2", which may never come.
inline constexpr uint32_t SCAN_MODE_WAIT_MS = 1500;
// How long a scan or connect may run without the module finishing it.
inline constexpr uint32_t SCAN_TIMEOUT_MS = 20000;
inline constexpr uint32_t CONNECT_TIMEOUT_MS = 15000;

inline constexpr size_t ADDRESS_LENGTH = 12;  // hex digits, no separators
inline constexpr size_t NAME_CAPACITY = 32;   // including the terminator

enum class EventType : uint8_t {
  None,
  ModeSet,
  ScanStarted,
  DeviceFound,
  ScanDone,
  Connecting,
  Connected,
  ConnectTimeout,
  Reconnect,
  Disconnected,  // "REMOTE DISCON": the headset dropped the link
};

struct Event {
  EventType type = EventType::None;
  uint8_t mode = 0;                       // ModeSet only
  char address[ADDRESS_LENGTH + 1] = {};  // DeviceFound only
  char name[NAME_CAPACITY] = {};          // DeviceFound only; may be empty
};

// Splits the UART byte stream into lines without allocating. Lines longer
// than the buffer are dropped whole rather than parsed truncated.
class LineReader {
 public:
  // Returns true when `c` completed a non-empty line, readable via line()
  // until the next feed().
  bool feed(char c);
  // Ends the current partial line, for replies sent without a line end.
  // Returns true when that made a non-empty line readable via line().
  bool flush();
  bool hasPartial() const { return !pendingReset_ && length_ > 0; }
  const char* line() const { return buffer_; }

 private:
  static constexpr size_t CAPACITY = 96;
  char buffer_[CAPACITY] = {};
  size_t length_ = 0;
  bool overflowed_ = false;
  bool pendingReset_ = false;
};

// Classifies one reply line. Unknown lines return EventType::None.
Event parseLine(const char* line);

bool isValidAddress(const char* address);

// Writes "AT+CONNECT=<address>\r\n" into `out`. Returns the length written,
// or 0 when the address is invalid or `capacity` is too small.
size_t formatConnect(const char* address, char* out, size_t capacity);

}  // namespace metalio_bt
