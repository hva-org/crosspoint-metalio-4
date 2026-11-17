#include "BtHeadphonesActivity.h"

#if CROSSPOINT_AUDIOBOOK

#include <HalAudio.h>
#include <I18n.h>

#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "audio/AudioOutput.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
enum class MenuRow : int16_t { Output, Scan, Saved, TestSound, ModuleLog };
constexpr int16_t SCAN_AGAIN = -1;
constexpr int16_t INFO_ROW = -2;
constexpr uint32_t TEST_TONE_MS = 1000;
// A scan reports devices one at a time; each e-ink repaint redraws the list.
constexpr unsigned long STATUS_REPAINT_MS = 3000;
// A headset reconnect inside the clock check can take this long.
constexpr unsigned long TONE_WAIT_MS = 45000;

bool headphonesSelected() { return halAudio.output() == HalAudio::Output::Headphones; }
}  // namespace

void BtHeadphonesActivity::onEnter() {
  UiListActivity::onEnter();
  audioOpen = audiobook::acquireOutput();
  refresh();
  requestUpdate();
}

void BtHeadphonesActivity::onExit() {
  if (audioOpen) audiobook::releaseOutput();
  audioOpen = false;
  UiListActivity::onExit();
}

void BtHeadphonesActivity::loop() {
  UiListActivity::loop();
  if (pickedAddress[0] != '\0' && halAudio.link() == HalAudio::Link::Connected &&
      strcmp(halAudio.connectedAddress(), pickedAddress) == 0) {
    saveConnectedHeadset();
  }
  if (toneWaiting) {
    if (halAudio.clockConfirmed()) {
      toneWaiting = false;
      // Blocks input for about a second; the render task keeps running.
      halAudio.playTestTone(TEST_TONE_MS, SETTINGS.audio.volume);
    } else if (halAudio.wakeFailed() || millis() - toneSinceMs > TONE_WAIT_MS) {
      toneWaiting = false;  // the Module Log says which step failed
    }
  }
  if (halAudio.revision() == shownRevision) return;
  if (millis() - lastRepaintMs < STATUS_REPAINT_MS) return;
  refresh();
  requestUpdate();
}

const char* BtHeadphonesActivity::headerTitle() const {
  switch (view) {
    case View::Scan:
      return tr(STR_AUDIOBOOK_SCAN);
    case View::Log:
      return tr(STR_AUDIOBOOK_MODULE_LOG);
    case View::Menu:
      break;
  }
  return tr(STR_AUDIOBOOK_HEADPHONES_SETTINGS);
}

void BtHeadphonesActivity::onBackButton() {
  if (view != View::Menu) {
    showView(View::Menu);
    return;
  }
  finish();
}

void BtHeadphonesActivity::showView(const View target) {
  {
    RenderLock lock(*this);
    view = target;
    nav.reset();
  }
  refresh();
  requestUpdate();
}

void BtHeadphonesActivity::saveConnectedHeadset() {
  auto& audio = SETTINGS.audio;
  strncpy(audio.headsetAddress, pickedAddress, sizeof(audio.headsetAddress) - 1);
  audio.headsetAddress[sizeof(audio.headsetAddress) - 1] = '\0';
  audio.headsetName[0] = '\0';
  for (size_t i = 0; i < halAudio.deviceCount(); i++) {
    const auto& d = halAudio.device(i);
    if (strcmp(d.address, pickedAddress) == 0) {
      strncpy(audio.headsetName, d.name, sizeof(audio.headsetName) - 1);
      audio.headsetName[sizeof(audio.headsetName) - 1] = '\0';
    }
  }
  audio.output = audiobook::AudioSettings::OUTPUT_HEADPHONES;
  SETTINGS.saveToFile();
  pickedAddress[0] = '\0';
}

void BtHeadphonesActivity::refresh() {
  RenderLock lock(*this);
  shownRevision = halAudio.revision();
  lastRepaintMs = millis();
  const auto& saved = SETTINGS.audio;
  const bool haveSaved = saved.headsetAddress[0] != '\0';

  if (!audioOpen) {
    status = tr(STR_AUDIOBOOK_START_FAILED);
  } else if (!headphonesSelected()) {
    // The row's value already says Speaker.
    if (halAudio.modeSettled()) {
      status.clear();
    } else {
      status = tr(STR_AUDIOBOOK_SWITCHING);
    }
  } else {
    // Headset mode is entered by the connect or scan that needs it.
    switch (halAudio.link()) {
      case HalAudio::Link::Connected:
        status = haveSaved && strcmp(halAudio.connectedAddress(), saved.headsetAddress) == 0 && saved.headsetName[0]
                     ? saved.headsetName
                     : halAudio.connectedAddress();
        break;
      case HalAudio::Link::Connecting:
        status = tr(STR_CONNECTING);
        break;
      case HalAudio::Link::Scanning:
        status = tr(STR_SCANNING);
        break;
      case HalAudio::Link::Failed:
        status = tr(STR_CONNECTION_FAILED);
        break;
      case HalAudio::Link::None:
        status = haveSaved ? tr(STR_AUDIOBOOK_NOT_CONNECTED) : tr(STR_AUDIOBOOK_PICK_HEADPHONES);
        break;
    }
  }

  // Labels first: the rows point into them.
  labels.clear();
  const size_t found = view == View::Scan ? halAudio.deviceCount() : 0;
  for (size_t i = 0; i < found; i++) {
    const auto& d = halAudio.device(i);
    labels.emplace_back(d.name[0] != '\0' ? d.name : d.address);
  }
  if (view == View::Menu && haveSaved)
    labels.emplace_back(saved.headsetName[0] ? saved.headsetName : saved.headsetAddress);
  values.clear();
  if (view == View::Log) {
    // The last check by a write of silence (Test Sound or playback start).
    const auto clock = halAudio.lastClock();
    labels.emplace_back(clock == HalAudio::Clock::Running   ? tr(STR_AUDIOBOOK_CLOCK_RUNNING)
                        : clock == HalAudio::Clock::Missing ? tr(STR_AUDIOBOOK_CLOCK_STOPPED)
                                                            : tr(STR_AUDIOBOOK_CLOCK_UNCHECKED));
    values.emplace_back();
    char text[64];
    snprintf(text, sizeof(text), tr(STR_AUDIOBOOK_MODULE_RX), static_cast<unsigned>(halAudio.rxBytes()),
             static_cast<unsigned>(halAudio.rxLines()));
    labels.emplace_back(text);
    values.emplace_back();
    for (size_t i = 0; i < halAudio.logCount(); i++) {
      const auto& entry = halAudio.logEntry(i);
      snprintf(text, sizeof(text), "%c %s", entry.direction, entry.text);
      labels.emplace_back(text);
      snprintf(text, sizeof(text), "%u.%us", static_cast<unsigned>(entry.ms / 1000),
               static_cast<unsigned>(entry.ms % 1000 / 100));
      values.emplace_back(text);
    }
  }

  rows.clear();
  const auto add = [this](const char* label, const int16_t code, const bool enabled = true) -> fui::ListItem& {
    fui::ListItem row;
    row.label = label;
    row.actionValue = code;
    row.enabled = enabled;
    rows.push_back(row);
    return rows.back();
  };
  const bool scanning = halAudio.link() == HalAudio::Link::Scanning;
  // While a scan or connect runs, rows that send module commands wait.
  const bool busy = halAudio.isBusy();
  // Test Sound first checks the clock, which waits for the link or mode.
  const bool outputSetUp = headphonesSelected() ? halAudio.headsetConnected() : halAudio.modeSettled();
  switch (view) {
    case View::Menu: {
      // Output and Forget cancel a connect, so only a running scan holds them.
      auto& output = add(tr(STR_AUDIOBOOK_OUTPUT), static_cast<int16_t>(MenuRow::Output), audioOpen && !scanning);
      output.value = headphonesSelected() ? tr(STR_AUDIOBOOK_HEADPHONES) : tr(STR_AUDIOBOOK_SPEAKER);
      output.subtitle = status.empty() ? nullptr : status.c_str();
      add(tr(STR_AUDIOBOOK_SCAN), static_cast<int16_t>(MenuRow::Scan), audioOpen && headphonesSelected() && !busy);
      auto& savedRow = add(haveSaved ? labels.back().c_str() : tr(STR_AUDIOBOOK_SAVED_HEADPHONES),
                           static_cast<int16_t>(MenuRow::Saved), haveSaved && !scanning);
      savedRow.value = haveSaved ? tr(STR_FORGET_BUTTON) : tr(STR_NOT_SET);
      // A playing book owns the stream; the tone would not start.
      add(tr(STR_AUDIOBOOK_TEST_SOUND), static_cast<int16_t>(MenuRow::TestSound),
          audioOpen && outputSetUp && !toneWaiting && !halAudio.isStreaming());
      add(tr(STR_AUDIOBOOK_MODULE_LOG), static_cast<int16_t>(MenuRow::ModuleLog), audioOpen);
      break;
    }
    case View::Log:
      // Newest first, under the received-bytes summary. Rows do nothing.
      for (size_t i = 0; i < labels.size(); i++) {
        auto& row = add(labels[i].c_str(), INFO_ROW);
        if (!values[i].empty()) row.value = values[i].c_str();
      }
      break;
    case View::Scan: {
      add(tr(STR_AUDIOBOOK_SCAN), SCAN_AGAIN, audioOpen && !busy);
      for (size_t i = 0; i < found; i++) add(labels[i].c_str(), static_cast<int16_t>(i), !busy);
      if (found == 0) add(scanning ? tr(STR_SCANNING) : tr(STR_AUDIOBOOK_NO_HEADPHONES), INFO_ROW, false);
      break;
    }
  }
  if (nav.selected >= static_cast<int>(rows.size())) nav.selected = static_cast<int>(rows.size()) - 1;
}

void BtHeadphonesActivity::activateMenu(const int code) {
  auto& audio = SETTINGS.audio;
  switch (static_cast<MenuRow>(code)) {
    case MenuRow::Output: {
      if (halAudio.link() == HalAudio::Link::Scanning) break;  // drawn before the scan began
      const bool toHeadphones = !headphonesSelected();
      audio.output =
          toHeadphones ? audiobook::AudioSettings::OUTPUT_HEADPHONES : audiobook::AudioSettings::OUTPUT_SPEAKER;
      SETTINGS.saveToFile();
      halAudio.selectOutput(toHeadphones ? HalAudio::Output::Headphones : HalAudio::Output::Speaker,
                            audio.headsetAddress);
      break;
    }
    case MenuRow::Scan:
      halAudio.startScan();
      showView(View::Scan);
      return;
    case MenuRow::Saved:
      if (halAudio.link() == HalAudio::Link::Scanning) break;
      audio.headsetAddress[0] = '\0';
      audio.headsetName[0] = '\0';
      // Back to the speaker, as the reference firmware does. That also drops
      // the headset link, so the headphones can be found by a scan again.
      audio.output = audiobook::AudioSettings::OUTPUT_SPEAKER;
      SETTINGS.saveToFile();
      halAudio.selectOutput(HalAudio::Output::Speaker, nullptr);
      break;
    case MenuRow::TestSound:
      // loop() plays it once a check finds the module clocking the bus.
      halAudio.resetWake();
      halAudio.wakeOutput();
      toneWaiting = true;
      toneSinceMs = millis();
      break;
    case MenuRow::ModuleLog:
      showView(View::Log);
      return;
  }
  refresh();
  requestUpdate();
}

void BtHeadphonesActivity::activateIndex(const int index) {
  if (index < 0 || index >= static_cast<int>(rows.size()) || !rows[index].enabled) return;
  nav.selected = index;
  app.clearTapFlash();
  const int16_t code = rows[index].actionValue;
  if (view == View::Menu) {
    activateMenu(code);
    return;
  }
  if (view == View::Log) return;
  if (code == SCAN_AGAIN) {
    halAudio.startScan();
  } else if (code >= 0 && static_cast<size_t>(code) < halAudio.deviceCount()) {
    strncpy(pickedAddress, halAudio.device(code).address, sizeof(pickedAddress) - 1);
    halAudio.connect(pickedAddress);
    showView(View::Menu);
    return;
  }
  refresh();
  requestUpdate();
}

void BtHeadphonesActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.rowProvider = [](void* ctx, const uint16_t index, fui::ListItem& item) {
    item = static_cast<BtHeadphonesActivity*>(ctx)->rows[index];
    // UiListActivity dispatches row indices; the row codes stay in rows.
    item.actionValue = static_cast<int16_t>(index);
  };
  props.rowProviderCtx = this;
  props.count = static_cast<uint16_t>(rows.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  screen.list(props);
}

#endif
