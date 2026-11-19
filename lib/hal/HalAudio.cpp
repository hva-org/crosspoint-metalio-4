#include "HalAudio.h"

#if CROSSPOINT_AUDIOBOOK

#include <Logging.h>
#include <MetalioAudio.h>

#include <cmath>
#include <cstdio>
#include <cstring>

static_assert(FREEINK_DEVICE_METALIO_EINK4 && FREEINK_CAP_AUDIO,
              "CROSSPOINT_AUDIOBOOK drives the Metalio E-Ink 4 Bluetooth audio module");

namespace btmod = metalio_bt;
namespace metalio = freeink::metalio;

HalAudio halAudio;

namespace {
constexpr uint32_t WRITE_TIMEOUT_MS = 100;
constexpr size_t WRITE_FRAMES = HalAudio::WRITE_FRAMES;
// UART2 to the module, as in the SDK's acquireAudio().
constexpr int MODULE_UART_RX_PIN = 47;
constexpr int MODULE_UART_TX_PIN = 48;
constexpr uint32_t MODULE_UART_BAUD = 115200;
}  // namespace

// The I2S part of the SDK's metalio::acquireAudio() (freeink-sdk
// MetalioAudio.h): slave channels clocked by the module. Keep in step with it.
bool HalAudio::openI2s() {
  auto& bus = metalio::audioBus();
  if (bus.tx) return true;
  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_SLAVE);
  chan.auto_clear = true;
  if (i2s_new_channel(&chan, &bus.tx, &bus.rx) != ESP_OK) {
    bus.tx = bus.rx = nullptr;
    return false;
  }
  i2s_std_config_t cfg = {};
  cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(metalio::AUDIO_RATE);
  cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
  cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  cfg.gpio_cfg.bclk = static_cast<gpio_num_t>(BoardConfig::ACTIVE.audio.bclk);
  cfg.gpio_cfg.ws = static_cast<gpio_num_t>(BoardConfig::ACTIVE.audio.lrclk);
  cfg.gpio_cfg.dout = static_cast<gpio_num_t>(BoardConfig::ACTIVE.audio.dout);
  cfg.gpio_cfg.din = static_cast<gpio_num_t>(BoardConfig::ACTIVE.mic.data);
  if (i2s_channel_init_std_mode(bus.tx, &cfg) != ESP_OK || i2s_channel_init_std_mode(bus.rx, &cfg) != ESP_OK) {
    closeI2s();
    return false;
  }
  return true;
}

// Deletes the I2S channels but keeps UART2 and the bus ownership.
void HalAudio::closeI2s() {
  auto& bus = metalio::audioBus();
  if (bus.microphone) return;  // shared with a microphone owner; not used in this build
  // A writer stuck past stopStream()'s wait still holds the TX handle; keep
  // the channels until it leaves write().
  if (writing_.load()) return;
  if (bus.txEnabled) i2s_channel_disable(bus.tx);
  if (bus.rxEnabled) i2s_channel_disable(bus.rx);
  bus.txEnabled = bus.rxEnabled = false;
  if (bus.tx) i2s_del_channel(bus.tx);
  if (bus.rx) i2s_del_channel(bus.rx);
  bus.tx = bus.rx = nullptr;
}

// Opens UART2 and the I2S bus the way the SDK's acquireAudio() does, minus
// its fixed speaker commands, which would drop a headset link and reach the
// module just before our own mode commands. acquireAudio() then only takes
// ownership of the open bus.
bool HalAudio::openBus() {
  auto& bus = metalio::audioBus();
  if (bus.tx) return true;  // already open for another owner
  if (!metalio::ensureBooted() || !metalio::setAmplifier(false)) return false;
  bus.uart.begin(MODULE_UART_BAUD, SERIAL_8N1, MODULE_UART_RX_PIN, MODULE_UART_TX_PIN);
  if (!openI2s()) {
    metalio::destroyAudioBus();
    return false;
  }
  return true;
}

bool HalAudio::begin() {
  // Its own flag, not bus.tx: the channels are deleted between streams.
  if (open_) return true;
  // With the bus open, acquireAudio() only takes ownership; it would
  // otherwise send the speaker commands and block for 700 ms.
  if (!openBus() || !metalio::acquireAudio(this, false)) {
    LOG_ERR("AUD", "Audio bus unavailable");
    if (!metalio::audioBus().speaker && !metalio::audioBus().microphone) metalio::destroyAudioBus();
    return false;
  }
  // The channels are created again for each stream; none are held while idle.
  closeI2s();
  open_ = true;
  link_ = Link::None;
  clearQueue();
  deviceCount_ = 0;
  forgetClock();
  wake_ = Wake::Idle;
  revision_++;
  return true;
}

bool HalAudio::modeSettled() const {
  const ModuleMode wanted = output_ == Output::Headphones ? ModuleMode::Headset : ModuleMode::Speaker;
  return moduleMode_ == wanted && queuedMode_ == wanted && static_cast<int32_t>(millis() - settledAtMs_) >= 0;
}

bool HalAudio::headsetReady() const {
  return headsetConnected_ && musicSentMs_ != 0 && stepCount_ == 0 &&
         static_cast<int32_t>(millis() - musicSentMs_) >= static_cast<int32_t>(MUSIC_SETTLE_MS);
}

void HalAudio::sendNow(const char* text) {
  auto& uart = metalio::audioBus().uart;
  uart.print(text);
  uart.flush();  // delays count from when the command has left
  LOG_DBG("AUD", "-> %.*s", static_cast<int>(strcspn(text, "\r\n")), text);
  addLog('>', text);
}

void HalAudio::addLog(const char direction, const char* text) {
  LogEntry& entry = log_[logHead_];
  entry.ms = millis();
  entry.direction = direction;
  size_t n = 0;
  for (; text[n] != '\0' && text[n] != '\r' && text[n] != '\n' && n < sizeof(entry.text) - 1; n++) {
    // Printable ASCII only: a wrong baud rate shows up as '?' rather than as
    // bytes the font cannot draw.
    const auto c = static_cast<unsigned char>(text[n]);
    entry.text[n] = c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?';
  }
  entry.text[n] = '\0';
  logHead_ = (logHead_ + 1) % LOG_SIZE;
  if (logCount_ < LOG_SIZE) logCount_++;
  logRevision_++;
}

void HalAudio::receiveLine(const char* line) {
  LOG_DBG("AUD", "<- %s", line);
  rxLines_++;
  addLog('<', line);
  handle(btmod::parseLine(line));
}

void HalAudio::queue(const char* text, const uint16_t delayAfterMs, const Action action) {
  if (stepCount_ >= MAX_STEPS) {
    LOG_ERR("AUD", "Module command queue full");
    return;
  }
  Step& step = steps_[(stepHead_ + stepCount_) % MAX_STEPS];
  strncpy(step.text, text, sizeof(step.text) - 1);
  step.text[sizeof(step.text) - 1] = '\0';
  step.delayAfterMs = delayAfterMs;
  step.action = action;
  stepCount_++;
}

void HalAudio::clearQueue() {
  if (stepCount_ > 0) {
    // A mode command at the head means its role command already went out: the
    // module is now between modes, so the next request sends both again.
    const Action head = steps_[stepHead_].action;
    if (head == Action::SpeakerMode || head == Action::HeadsetMode || head == Action::HeadsetModeForScan) {
      moduleMode_ = ModuleMode::Unknown;
      headsetConnected_ = false;
    }
  }
  stepCount_ = 0;
  queuedMode_ = moduleMode_;
  scanWaitsForMode_ = false;
  connectPending_ = false;
}

void HalAudio::holdForModeGap() {
  if (stepCount_ > 0 || lastModeSentMs_ == 0) return;  // queued steps already keep their order
  const uint32_t earliest = lastModeSentMs_ + btmod::MODE_GAP_MS;
  if (static_cast<int32_t>(earliest - nextStepMs_) > 0) nextStepMs_ = earliest;
}

void HalAudio::queueSpeakerMode() {
  holdForModeGap();
  queue(btmod::CMD_SPEAKER_ROLE, btmod::ROLE_TO_MODE_DELAY_MS, Action::Role);
  queue(btmod::CMD_SPEAKER_MODE, 0, Action::SpeakerMode);
  queuedMode_ = ModuleMode::Speaker;
}

void HalAudio::queueHeadsetMode(const bool forScan) {
  holdForModeGap();
  queue(btmod::CMD_HEADSET_ROLE, btmod::ROLE_TO_MODE_DELAY_MS, Action::Role);
  queue(btmod::CMD_HEADSET_MODE, forScan ? btmod::SCAN_MODE_WAIT_MS : btmod::HEADSET_MODE_SETTLE_MS,
        forScan ? Action::HeadsetModeForScan : Action::HeadsetMode);
  queuedMode_ = ModuleMode::Headset;
}

void HalAudio::selectOutput(const Output output, const char* headsetAddress) {
  if (!open_) return;
  if (output == output_) {
    // Clearing the queue here could drop a queued AT+PP=1 or a mode switch.
    if (output == Output::Speaker && queuedMode_ == ModuleMode::Speaker) return;
    // Linked, connecting (ours or the module's own retry), or scanning.
    if (output == Output::Headphones && (headsetConnected_ || isBusy() || link_ == Link::Connecting)) return;
  }
  // A connect still running may answer after the switch to the speaker.
  const bool connectRunning = connectPending_ || link_ == Link::Connecting;
  clearQueue();
  output_ = output;
  wake_ = Wake::Idle;
  forgetClock();
  lateConnectWatch_ = output == Output::Speaker && connectRunning;
  lateConnectUntilMs_ = millis() + btmod::CONNECT_TIMEOUT_MS;
  if (output == Output::Speaker) {
    setLink(Link::None);
    if (moduleMode_ != ModuleMode::Speaker) {
      queueSpeakerMode();
    } else if (streaming_.load()) {
      metalio::setAmplifier(true);
    }
  } else {
    if (streaming_.load()) metalio::setAmplifier(false);
    if (moduleMode_ == ModuleMode::Headset && headsetConnected_) {
      setLink(Link::Connected);
    } else {
      setLink(Link::None);
      // Without a saved headset the mode changes on the first scan.
      if (headsetAddress && btmod::isValidAddress(headsetAddress)) connect(headsetAddress);
    }
  }
  revision_++;
}

void HalAudio::renewSpeakerMode() {
  if (!open_ || output_ != Output::Speaker) return;
  // A switch to the speaker still queued sends these commands anyway.
  if (queuedMode_ == ModuleMode::Speaker && moduleMode_ != ModuleMode::Speaker) return;
  clearQueue();
  moduleMode_ = ModuleMode::Unknown;
  queuedMode_ = ModuleMode::Unknown;
  forgetClock();
  queueSpeakerMode();
  revision_++;
}

void HalAudio::startScan() {
  // One scan or connect at a time, as in the reference firmware.
  if (!open_ || output_ != Output::Headphones || isBusy()) return;
  clearQueue();
  wake_ = Wake::Idle;
  deviceCount_ = 0;
  // Every scan re-enters headset mode first, as the reference firmware does.
  queueHeadsetMode(true);
  queue(btmod::CMD_SCAN, 0, Action::Scan);
  setLink(Link::Scanning);
  revision_++;
}

void HalAudio::connect(const char* address) {
  char command[32];
  if (!open_ || output_ != Output::Headphones || isBusy() ||
      btmod::formatConnect(address, command, sizeof(command)) == 0)
    return;
  if ((headsetConnected_ || link_ == Link::Connecting) && strcmp(connected_.address, address) == 0) return;
  if (queuedMode_ != ModuleMode::Headset) queueHeadsetMode(false);
  headsetConnected_ = false;
  musicSentMs_ = 0;
  forgetClock();
  connected_ = Device{};
  for (size_t i = 0; i < deviceCount_; i++) {
    if (strcmp(devices_[i].address, address) == 0) connected_ = devices_[i];
  }
  strncpy(connected_.address, address, sizeof(connected_.address) - 1);
  queue(command, 0, Action::Connect);
  connectPending_ = true;
  setLink(Link::Connecting);
  revision_++;
}

void HalAudio::dropHeadsetLink() {
  headsetConnected_ = false;
  musicSentMs_ = 0;
  forgetClock();
  if (link_ == Link::Connected) setLink(Link::None);
  revision_++;
}

void HalAudio::setLink(const Link link) {
  link_ = link;
  linkSinceMs_ = millis();
}

void HalAudio::sendStep() {
  const Step step = steps_[stepHead_];
  stepHead_ = (stepHead_ + 1) % MAX_STEPS;
  stepCount_--;
  sendNow(step.text);
  const uint32_t now = millis();
  nextStepMs_ = now + step.delayAfterMs;
  switch (step.action) {
    case Action::SpeakerMode:
      lastModeSentMs_ = now;
      moduleMode_ = ModuleMode::Speaker;
      headsetConnected_ = false;
      settledAtMs_ = now + btmod::SPEAKER_MODE_SETTLE_MS;
      settleShown_ = false;
      forgetClock();
      if (output_ == Output::Speaker && streaming_.load()) metalio::setAmplifier(true);
      break;
    case Action::HeadsetMode:
    case Action::HeadsetModeForScan:
      // A mode switch drops any headset link.
      lastModeSentMs_ = now;
      moduleMode_ = ModuleMode::Headset;
      headsetConnected_ = false;
      musicSentMs_ = 0;
      settledAtMs_ = nextStepMs_;
      forgetClock();
      scanWaitsForMode_ = step.action == Action::HeadsetModeForScan;
      break;
    case Action::Scan:
    case Action::Connect:
      setLink(step.action == Action::Scan ? Link::Scanning : Link::Connecting);  // deadline from now
      break;
    case Action::Role:
      lastModeSentMs_ = now;  // a cancelled switch still counts for the gap
      break;
    case Action::MusicStart:
      musicSentMs_ = now == 0 ? 1 : now;
      break;
    case Action::None:
    case Action::MusicProfile:
      break;
  }
  revision_++;
}

void HalAudio::handle(const btmod::Event& event) {
  switch (event.type) {
    case btmod::EventType::ModeSet:
      // Only the scan waits for this, and only up to SCAN_MODE_WAIT_MS.
      if (event.mode == 2 && scanWaitsForMode_) {
        scanWaitsForMode_ = false;
        nextStepMs_ = millis();
        settledAtMs_ = nextStepMs_;
      }
      break;
    case btmod::EventType::ScanStarted:
      break;
    case btmod::EventType::DeviceFound: {
      bool known = false;
      for (size_t i = 0; i < deviceCount_; i++) {
        if (strcmp(devices_[i].address, event.address) == 0) known = true;
      }
      if (!known && deviceCount_ < MAX_DEVICES) {
        Device& d = devices_[deviceCount_++];
        memcpy(d.address, event.address, sizeof(d.address));
        memcpy(d.name, event.name, sizeof(d.name));
      }
      break;
    }
    case btmod::EventType::ScanDone:
      if (link_ == Link::Scanning) setLink(headsetConnected_ ? Link::Connected : Link::None);
      break;
    case btmod::EventType::Connecting:
    case btmod::EventType::Reconnect:
      // Also seen when the module retries a link it lost: nothing is heard
      // until it succeeds. Repeats do not restart the deadline.
      headsetConnected_ = false;
      musicSentMs_ = 0;
      forgetClock();
      if (link_ != Link::Scanning && link_ != Link::Connecting) setLink(Link::Connecting);
      break;
    case btmod::EventType::Connected:
      connectPending_ = false;
      if (headsetConnected_) break;  // AT+PP=1 toggles: one per link, or it pauses
      if (output_ != Output::Headphones) {
        // A connect sent before the user chose the speaker. A speaker switch
        // still queued drops this link; one already sent may not have, so the
        // module could be sending audio to the headset: switch again, once.
        // Other links in speaker mode do not repeat the switch.
        const bool expected = lateConnectWatch_ && static_cast<int32_t>(millis() - lateConnectUntilMs_) < 0;
        lateConnectWatch_ = false;
        if (expected && moduleMode_ == ModuleMode::Speaker) {
          addLog('!', "late connect: speaker again");
          renewSpeakerMode();
        } else {
          addLog('!', "late connect ignored");
        }
        break;
      }
      // A late reply to an earlier connect does not end a running scan.
      if (link_ != Link::Scanning) setLink(Link::Connected);
      headsetConnected_ = true;
      musicSentMs_ = 0;
      forgetClock();
      queue(btmod::CMD_MUSIC_PROFILE, btmod::MUSIC_STEP_DELAY_MS, Action::MusicProfile);
      queue(btmod::CMD_MUSIC_START, 0, Action::MusicStart);
      break;
    case btmod::EventType::Disconnected:
      headsetConnected_ = false;
      musicSentMs_ = 0;
      forgetClock();
      if (link_ == Link::Connected) setLink(Link::None);
      break;
    case btmod::EventType::ConnectTimeout:
      if (link_ != Link::Scanning) setLink(Link::Failed);
      headsetConnected_ = false;
      connectPending_ = false;
      forgetClock();
      break;
    case btmod::EventType::None:
      return;
  }
  revision_++;
}

void HalAudio::poll() {
  if (!open_) return;
  auto& uart = metalio::audioBus().uart;
  const uint32_t now = millis();
  const uint32_t bytesBefore = rxBytes_;
  while (uart.available() > 0) {
    rxBytes_++;
    lastRxMs_ = now;
    if (reader_.feed(static_cast<char>(uart.read()))) receiveLine(reader_.line());
  }
  if (reader_.hasPartial() && now - lastRxMs_ > PARTIAL_LINE_MS && reader_.flush()) receiveLine(reader_.line());
  if (rxBytes_ != bytesBefore) logRevision_++;  // the Module Log counts bytes, even of dropped lines

  if (stepCount_ > 0 && static_cast<int32_t>(now - nextStepMs_) >= 0) sendStep();
  if (!settleShown_ && static_cast<int32_t>(now - settledAtMs_) >= 0) {
    settleShown_ = true;
    revision_++;  // screens showing "Switching" redraw
  }
  pollWake(now);

  // Signed: setLink() during this poll may have read a later millis() than `now`.
  const int32_t linkAgeMs = static_cast<int32_t>(now - linkSinceMs_);
  if (link_ == Link::Scanning && linkAgeMs > static_cast<int32_t>(btmod::SCAN_TIMEOUT_MS)) {
    LOG_ERR("AUD", "Scan got no INQ COMPLETE");
    setLink(headsetConnected_ ? Link::Connected : Link::None);
    revision_++;
  } else if (link_ == Link::Connecting && linkAgeMs > static_cast<int32_t>(btmod::CONNECT_TIMEOUT_MS)) {
    LOG_ERR("AUD", "Connect got no reply");
    setLink(Link::Failed);
    headsetConnected_ = false;
    connectPending_ = false;
    revision_++;
  }
}

void HalAudio::wakeOutput() {
  if (!open_ || streaming_.load() || clockConfirmed() || wake_ == Wake::Trying) return;
  wake_ = Wake::Trying;
  wakeStep_ = 0;  // step 0 only checks
  wakeCheckAtMs_ = millis();
  wakeDeadlineMs_ = wakeCheckAtMs_ + WAKE_READY_MS;
  revision_++;
}

void HalAudio::resetWake() {
  if (wake_ == Wake::Failed) wake_ = Wake::Idle;
}

void HalAudio::cancelWake() {
  if (wake_ == Wake::Idle) return;
  wake_ = Wake::Idle;
  revision_++;
}

bool HalAudio::probeClock() {
  if (!open_ || streaming_.load()) return false;
  closeI2s();
  if (!openI2s() || !metalio::startAudio(this, false)) {
    closeI2s();
    return false;
  }
  static const int32_t silence[WRITE_FRAMES * 2] = {};
  auto tx = metalio::audioBus().tx;
  size_t frames = 0;
  while (frames < PROBE_FRAMES) {
    size_t written = 0;
    i2s_channel_write(tx, silence, sizeof(silence), &written, WRITE_TIMEOUT_MS);
    frames += written / (2 * sizeof(int32_t));
    if (written < sizeof(silence)) break;
  }
  closeI2s();
  clockConfirmed_ = frames >= PROBE_FRAMES;
  lastClock_ = clockConfirmed_ ? Clock::Running : Clock::Missing;
  revision_++;
  return clockConfirmed_;
}

void HalAudio::pollWake(const uint32_t now) {
  if (wake_ != Wake::Trying) return;
  if (stepCount_ > 0) {
    wakeCheckAtMs_ = now + WAKE_CHECK_MS;  // check only after the last command went out
    return;
  }
  if (static_cast<int32_t>(now - wakeCheckAtMs_) < 0) return;
  const bool headphones = output_ == Output::Headphones;
  // Each step needs its mode, or a linked headset told to play, before a check.
  if (!(headphones ? headsetReady() : modeSettled())) {
    if (static_cast<int32_t>(now - wakeDeadlineMs_) >= 0 || (headphones && link_ == Link::Failed)) {
      addLog('!', headphones ? "wake: headset not ready" : "wake: speaker mode not set");
      wake_ = Wake::Failed;
      revision_++;
    }
    return;
  }
  char text[32];
  if (probeClock()) {
    wake_ = Wake::Idle;
    snprintf(text, sizeof(text), "clock ok (step %u)", static_cast<unsigned>(wakeStep_));
    addLog('~', text);
    return;
  }
  wakeStep_++;
  wakeCheckAtMs_ = now + WAKE_CHECK_MS;
  wakeDeadlineMs_ = now + WAKE_READY_MS;
  if (!headphones) {
    // Sending the speaker commands again is what restored the clock on the
    // device; a later check waits once more before giving up.
    if (wakeStep_ == 1) {
      addLog('!', "no clock: speaker mode again");
      renewSpeakerMode();
    } else if (wakeStep_ == 2) {
      addLog('!', "no clock: waiting");
    } else {
      addLog('!', "no clock after all steps");
      wake_ = Wake::Failed;
    }
    revision_++;
    return;
  }
  // No lone AT+PP=1 here: it toggles, and the module kept clocking a paused
  // stream on the device, so a check cannot tell which way it went.
  switch (wakeStep_) {
    case 1:
      addLog('!', "no clock: waiting");  // a stream just started may need a moment
      break;
    case 2: {
      // Headset mode as at boot, then a connect: the sequence after which the
      // device once played the test tone.
      addLog('!', "no clock: reconnect");
      char address[sizeof(connected_.address)];
      memcpy(address, connected_.address, sizeof(address));
      clearQueue();
      moduleMode_ = ModuleMode::Unknown;
      queuedMode_ = ModuleMode::Unknown;
      dropHeadsetLink();
      connect(address);
      wakeDeadlineMs_ = now + WAKE_RECONNECT_MS;
      break;
    }
    default:
      addLog('!', "no clock after all steps");
      wake_ = Wake::Failed;
      break;
  }
  revision_++;
}

bool HalAudio::startStream() {
  if (!open_) return false;
  if (streaming_.load()) return true;
  // Fresh channels for every stream, created while the module clocks in its
  // current mode.
  closeI2s();
  if (!openI2s() || !metalio::startAudio(this, false)) {
    LOG_ERR("AUD", "I2S start failed");
    closeI2s();
    return false;
  }
  // Prime silence before the amplifier comes up: enabling it on an idle line pops.
  static const int32_t silence[WRITE_FRAMES * 2] = {};
  size_t written = 0;
  i2s_channel_write(metalio::audioBus().tx, silence, sizeof(silence), &written, WRITE_TIMEOUT_MS);
  addLog('~', written == sizeof(silence) ? "stream start: clock ok" : "stream start: no clock");
  // Before speaker mode is set, sendStep() turns it on.
  const bool speakerReady = output_ == Output::Speaker && moduleMode_ == ModuleMode::Speaker;
  if (speakerReady && !metalio::setAmplifier(true)) LOG_ERR("AUD", "Amplifier control failed");
  streaming_.store(true);
  revision_++;
  return true;
}

void HalAudio::stopStream() {
  if (!streaming_.exchange(false)) return;
  metalio::setAmplifier(false);
  // A write ends within one 100 ms block once streaming_ is false.
  const uint32_t start = millis();
  while (writing_.load() && millis() - start < 3 * WRITE_TIMEOUT_MS) delay(1);
  metalio::stopAudio(this, false);
  if (writing_.load()) {
    LOG_ERR("AUD", "Writer still busy; I2S channels kept");  // deleted at the next stream start
  } else {
    closeI2s();
  }
  forgetClock();  // the module may stop clocking while idle; the next stream checks again
  revision_++;
}

size_t HalAudio::write(const int16_t* samples, const size_t count, const uint8_t volume) {
  // Set before checking streaming_: stopStream() clears that, then waits for
  // this to clear before it deletes the channel.
  writing_.store(true);
  if (!streaming_.load()) {
    writing_.store(false);
    return 0;
  }
  auto tx = metalio::audioBus().tx;
  int32_t* slots = slots_;
  size_t done = 0;
  while (done < count && streaming_.load()) {
    const size_t frames = count - done < WRITE_FRAMES ? count - done : WRITE_FRAMES;
    for (size_t i = 0; i < frames; i++) {
      const int32_t v = metalio::outputSample(samples[done + i], volume);
      slots[i * 2] = v;
      slots[i * 2 + 1] = v;
    }
    size_t written = 0;
    // ESP-IDF takes milliseconds here. A missing module clock times out
    // instead of hanging the audio task.
    const esp_err_t err = i2s_channel_write(tx, slots, frames * 2 * sizeof(int32_t), &written, WRITE_TIMEOUT_MS);
    done += written / (2 * sizeof(int32_t));
    if (err != ESP_OK || written < frames * 2 * sizeof(int32_t)) break;
  }
  writing_.store(false);
  return done;
}

bool HalAudio::playTestTone(const uint32_t durationMs, const uint8_t volume) {
  if (!open_ || streaming_.load() || !startStream()) return false;
  constexpr float TONE_HZ = 1000.0f;
  int16_t block[WRITE_FRAMES];
  const uint32_t total = SAMPLE_RATE * durationMs / 1000;
  uint32_t n = 0;
  bool ok = true;
  while (ok && n < total) {
    for (size_t i = 0; i < WRITE_FRAMES; i++, n++) {
      block[i] = static_cast<int16_t>(12000.0f * sinf(2.0f * 3.14159265f * TONE_HZ * n / SAMPLE_RATE));
    }
    ok = write(block, WRITE_FRAMES, volume) == WRITE_FRAMES;
  }
  stopStream();
  addLog('~', ok ? "test tone played" : "test tone: no clock");
  return ok;
}

#endif  // CROSSPOINT_AUDIOBOOK
