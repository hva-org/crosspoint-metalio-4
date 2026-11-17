#pragma once

// Metalio E-Ink 4 audio output through the board's Bluetooth audio module.
// The module clocks a 16 kHz I2S bus and plays it on the speaker (mode 1) or
// sends it to a Bluetooth headset (mode 2). It is driven by text commands on
// UART2 and stays powered, in its last mode, while the board runs.
//
// What the device showed:
// - Speaker sessions open the bus through the SDK, which sends the speaker
//   commands on every open; the speaker only played with that.
// - Headset commands follow a firmware that drives this module: no mode
//   command it does not need, and no step waits for a reply it may not send.
// - The I2S channels are created fresh for every stream, while the module
//   already clocks in its mode. A channel kept across a mode change stayed
//   silent until a restart.
// - Before a stream starts, a short write of silence checks that the module
//   clocks the bus; if not, wakeOutput() tries the commands that may restart
//   it, checking after each one.
//
// Threads: control calls (open/close, output, scan, connect, poll, start/stop
// stream, wake) belong to the main loop. write() belongs to one audio task,
// and stopStream() must not run while that task is inside write().

#include <BoardConfig.h>

#if CROSSPOINT_AUDIOBOOK

#include <BtModuleProtocol.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

class HalAudio {
 public:
  static constexpr uint32_t SAMPLE_RATE = 16000;
  static constexpr size_t MAX_DEVICES = 12;
  static constexpr size_t WRITE_FRAMES = 128;

  enum class Output : uint8_t { Speaker, Headphones };
  enum class Link : uint8_t { None, Scanning, Connecting, Connected, Failed };
  enum class Clock : uint8_t { Unchecked, Running, Missing };

  struct Device {
    char address[metalio_bt::ADDRESS_LENGTH + 1] = {};
    char name[metalio_bt::NAME_CAPACITY] = {};
  };

  // One command sent ('>'), reply received ('<') or note ('!', '~'), for the
  // Module Log screen.
  struct LogEntry {
    uint32_t ms = 0;
    char direction = ' ';
    char text[39] = {};
  };
  static constexpr size_t LOG_SIZE = 12;

  // Counted: each acquire() needs one release(). For the speaker the first
  // blocks about 700 ms while the SDK opens the bus with the speaker commands;
  // for headphones it sends nothing, and selectOutput() does what is needed.
  bool acquire(Output wanted);
  // The last release finishes a mode switch whose role command already went
  // out (up to 700 ms) and the music commands, drops anything else queued,
  // then frees the bus. The module keeps its mode and headset link.
  void release();
  bool isOpen() const { return users_ > 0; }

  // Puts the module in the mode for `output`, if it is not already. For
  // headphones with a saved address, also connects to that headset.
  void selectOutput(Output output, const char* headsetAddress);
  Output output() const { return output_; }
  // Headset mode, then a scan. Found devices are listed in device().
  void startScan();
  // Headset mode if needed, then a connect to `address`.
  void connect(const char* address);

  // Sends queued commands, parses module replies and runs wakeOutput().
  // Main loop only.
  void poll();
  // Changes whenever something a screen shows changes.
  uint32_t revision() const { return revision_; }

  Link link() const { return link_; }
  // The module reported a headset link that has not failed or been replaced
  // by a mode switch since. It outlives release().
  bool headsetConnected() const { return headsetConnected_; }
  // The headset is linked and the music commands have gone out.
  bool headsetReady() const;
  // A scan, or a connect this driver started, is running; another one would
  // overlap it. Links the module retries by itself do not count.
  bool isBusy() const { return link_ == Link::Scanning || connectPending_; }
  // The module was put in the selected output's mode and had time to switch.
  bool modeSettled() const;

  // Makes sure the module clocks the bus for the selected output before a
  // stream: a write of silence checks, and while it fails the next step is
  // taken, with a check after each. Headphones: wait, then a full headset
  // switch and reconnect (never a lone AT+PP=1, which toggles play/pause).
  // Speaker: the speaker commands again. Each step goes to the Module Log.
  // No-op while running or confirmed.
  void wakeOutput();
  // Lets a failed wake run again.
  void resetWake();
  bool clockConfirmed() const { return clockConfirmed_; }
  bool wakeFailed() const { return wake_ == Wake::Failed; }
  bool waking() const { return wake_ == Wake::Trying; }
  // The last check's result, for the Module Log; kept after its stream ends.
  Clock lastClock() const { return lastClock_; }

  // Adds a diagnostic line to the Module Log. Main loop only.
  void note(const char* text) { addLog('!', text); }
  // Newest first; index < logCount(). Kept since boot.
  size_t logCount() const { return logCount_; }
  const LogEntry& logEntry(size_t index) const { return log_[(logHead_ + LOG_SIZE - 1 - index) % LOG_SIZE]; }
  // Every byte and line received since boot.
  uint32_t rxBytes() const { return rxBytes_; }
  uint32_t rxLines() const { return rxLines_; }
  bool isStreaming() const { return streaming_.load(); }
  size_t deviceCount() const { return deviceCount_; }
  const Device& device(size_t index) const { return devices_[index]; }
  const char* connectedAddress() const { return connected_.address; }

  // Creates the I2S channels and starts them. The amplifier is enabled only
  // for speaker output in speaker mode.
  bool startStream();
  // Stops and deletes the I2S channels.
  void stopStream();
  // Writes 16 kHz mono samples at `volume` (0-100), blocking until the DMA
  // takes them. Returns the count written; less on a 100 ms clock timeout.
  size_t write(const int16_t* samples, size_t count, uint8_t volume);

  // Plays a short tone from the calling task, for checking the output path.
  bool playTestTone(uint32_t durationMs, uint8_t volume);

 private:
  enum class ModuleMode : uint8_t { Unknown, Speaker, Headset };
  // What sending a queued command means for the state above.
  enum class Action : uint8_t {
    None,
    Role,  // first command of a mode switch
    SpeakerMode,
    HeadsetMode,
    HeadsetModeForScan,
    Scan,
    Connect,
    MusicProfile,
    MusicStart,
  };
  enum class Wake : uint8_t { Idle, Trying, Failed };
  struct Step {
    char text[32] = {};
    uint16_t delayAfterMs = 0;
    Action action = Action::None;
  };
  static constexpr size_t MAX_STEPS = 6;
  // A reply without a line end is taken as complete after this much quiet.
  static constexpr uint32_t PARTIAL_LINE_MS = 200;
  static constexpr uint32_t MUSIC_SETTLE_MS = 300;
  static constexpr uint32_t WAKE_CHECK_MS = 1500;       // after each wake command
  static constexpr uint32_t WAKE_READY_MS = 10000;      // for the link or mode a step needs
  static constexpr uint32_t WAKE_RECONNECT_MS = 25000;  // the reconnect step
  // More than the I2S DMA ring holds (6 x 240 frames): success means the
  // module clocked the silence out.
  static constexpr size_t PROBE_FRAMES = 2048;

  bool openBus();
  bool openI2s();
  void closeI2s();
  bool probeClock();
  void pollWake(uint32_t now);
  void forgetClock() { clockConfirmed_ = false; }
  void queue(const char* text, uint16_t delayAfterMs, Action action = Action::None);
  void clearQueue();
  void queueSpeakerMode();
  void queueHeadsetMode(bool forScan);
  void holdForModeGap();
  void sendStep();
  void handle(const metalio_bt::Event& event);
  void setLink(Link link);
  void dropHeadsetLink();
  void sendNow(const char* text);
  void receiveLine(const char* line);
  void addLog(char direction, const char* text);

  uint8_t users_ = 0;
  Output output_ = Output::Speaker;
  Link link_ = Link::None;
  uint32_t linkSinceMs_ = 0;

  // Module state; it survives release() because the module stays powered.
  ModuleMode moduleMode_ = ModuleMode::Unknown;
  ModuleMode queuedMode_ = ModuleMode::Unknown;  // after the queued commands
  uint32_t settledAtMs_ = 0;
  bool settleShown_ = true;      // revision_ moved on when the settle time passed
  uint32_t lastModeSentMs_ = 0;  // 0: no mode command since boot
  bool headsetConnected_ = false;
  bool scanWaitsForMode_ = false;
  bool connectPending_ = false;  // our AT+CONNECT has no answer yet
  uint32_t musicSentMs_ = 0;     // AT+PP=1 sent for the current link; 0: not yet

  bool clockConfirmed_ = false;  // a check passed and nothing changed since
  Clock lastClock_ = Clock::Unchecked;
  Wake wake_ = Wake::Idle;
  uint8_t wakeStep_ = 0;
  uint32_t wakeCheckAtMs_ = 0;
  uint32_t wakeDeadlineMs_ = 0;

  Step steps_[MAX_STEPS];
  size_t stepHead_ = 0;
  size_t stepCount_ = 0;
  uint32_t nextStepMs_ = 0;

  metalio_bt::LineReader reader_;
  uint32_t lastRxMs_ = 0;
  uint32_t rxBytes_ = 0;
  uint32_t rxLines_ = 0;
  LogEntry log_[LOG_SIZE];
  size_t logHead_ = 0;
  size_t logCount_ = 0;
  Device devices_[MAX_DEVICES];
  size_t deviceCount_ = 0;
  Device connected_;
  uint32_t revision_ = 0;

  std::atomic<bool> streaming_{false};
  std::atomic<bool> writing_{false};  // the audio task is inside write()
  // Stereo 32-bit I2S slots for one write() block; only the single writer uses it.
  int32_t slots_[WRITE_FRAMES * 2] = {};
};

extern HalAudio halAudio;

#endif  // CROSSPOINT_AUDIOBOOK
