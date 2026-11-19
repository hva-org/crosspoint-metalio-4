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
enum class MenuRow : int16_t { Output, Saved, Forget, Scan, TestSound, ModuleLog };
constexpr int16_t SCAN_AGAIN = -1;
constexpr int16_t INFO_ROW = -2;
constexpr uint32_t TEST_TONE_MS = 1000;
// The Module Log changes with every byte the module sends.
constexpr unsigned long LOG_REFRESH_MS = 1000;
// A headset reconnect inside the clock check can take this long.
constexpr unsigned long TONE_WAIT_MS = 45000;

bool headphonesSelected() { return halAudio.output() == HalAudio::Output::Headphones; }

// FNV-1a over what the rows show, so the screen repaints only on a change.
uint32_t rowSignature(const std::vector<fui::ListItem>& rows) {
  uint32_t hash = 2166136261u;
  const auto mix = [&hash](const char* text) {
    for (const char* c = text ? text : ""; *c != '\0'; ++c) {
      hash = (hash ^ static_cast<uint8_t>(*c)) * 16777619u;
    }
    hash = (hash ^ 0xFFu) * 16777619u;  // field separator
  };
  for (const auto& row : rows) {
    mix(row.label);
    mix(row.value);
    mix(row.subtitle);
    hash = (hash ^ (row.enabled ? 1u : 2u)) * 16777619u;
  }
  return hash;
}
}  // namespace

void BtHeadphonesActivity::onEnter() {
  UiListActivity::onEnter();
  audioOpen = audiobook::ensureOutput();
  refresh();
  requestUpdate();
}

void BtHeadphonesActivity::onExit() {
  if (toneWaiting) halAudio.cancelWake();
  toneWaiting = false;
  // After a scan or a pick that did not connect; any way out of the screen.
  audiobook::applySavedOutput();
  UiListActivity::onExit();
}

void BtHeadphonesActivity::loop() {
  UiListActivity::loop();
  if (toneWaiting) {
    bool done = true;
    if (halAudio.clockConfirmed()) {
      // Blocks input for about a second; the render task keeps running.
      halAudio.playTestTone(TEST_TONE_MS, SETTINGS.audio.volume);
    } else if (halAudio.waking() && millis() - toneSinceMs <= TONE_WAIT_MS) {
      done = false;
    }
    // Otherwise it failed or was cancelled (output switched); the Module Log says which.
    if (done) {
      if (halAudio.waking()) halAudio.cancelWake();  // timed out
      toneWaiting = false;
      if (refresh()) requestUpdate();  // the row's enabled state changed
      return;
    }
  }
  const bool logDue =
      view == View::Log && halAudio.logRevision() != shownLogRevision && millis() - lastLogRefreshMs >= LOG_REFRESH_MS;
  if (halAudio.revision() == shownRevision && !logDue) return;
  if (refresh()) requestUpdate();
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
    // Leaving the scan without a pick: back to the saved output once it ends.
    if (view == View::Scan) audiobook::applySavedOutput();
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

void BtHeadphonesActivity::startScan() {
  audiobook::scanForHeadphones();
  showView(View::Scan);
}

bool BtHeadphonesActivity::refresh() {
  RenderLock lock(*this);
  shownRevision = halAudio.revision();
  shownLogRevision = halAudio.logRevision();
  lastLogRefreshMs = millis();
  const auto& saved = SETTINGS.audio;
  const bool haveSaved = saved.headsetAddress[0] != '\0';
  const bool onSaved =
      haveSaved && headphonesSelected() && strcmp(halAudio.connectedAddress(), saved.headsetAddress) == 0;

  status = audioOpen ? audiobook::outputStatus() : tr(STR_AUDIOBOOK_START_FAILED);

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
      // Tapping the saved headphones connects them; Forget has its own row.
      auto& savedRow = add(haveSaved ? labels.back().c_str() : tr(STR_AUDIOBOOK_SAVED_HEADPHONES),
                           static_cast<int16_t>(MenuRow::Saved), audioOpen && haveSaved && !busy);
      if (!haveSaved) {
        savedRow.value = tr(STR_NOT_SET);
      } else if (onSaved && halAudio.headsetConnected()) {
        savedRow.value = tr(STR_AUDIOBOOK_CONNECTED);
      } else if (onSaved && halAudio.link() == HalAudio::Link::Connecting) {
        savedRow.value = tr(STR_CONNECTING);
      } else {
        savedRow.value = tr(STR_CONNECT);
      }
      if (haveSaved) {
        add(tr(STR_AUDIOBOOK_FORGET_HEADPHONES), static_cast<int16_t>(MenuRow::Forget), audioOpen && !scanning);
      }
      add(tr(STR_AUDIOBOOK_SCAN), static_cast<int16_t>(MenuRow::Scan), audioOpen && !busy);
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
  const uint32_t signature = rowSignature(rows);
  const bool changed = signature != shownSignature;
  shownSignature = signature;
  return changed;
}

void BtHeadphonesActivity::activateMenu(const int code) {
  // Rows are drawn before a scan or connect began; these checks are current.
  const bool scanning = halAudio.link() == HalAudio::Link::Scanning;
  switch (static_cast<MenuRow>(code)) {
    case MenuRow::Output:
      if (scanning) break;
      if (headphonesSelected()) {
        audiobook::chooseOutput(HalAudio::Output::Speaker);
      } else if (SETTINGS.audio.headsetAddress[0] == '\0') {
        startScan();  // nothing to connect to yet
        return;
      } else {
        audiobook::chooseOutput(HalAudio::Output::Headphones);
      }
      break;
    case MenuRow::Saved:
      // Connects unless that headset is already linked or connecting.
      if (!halAudio.isBusy()) audiobook::chooseOutput(HalAudio::Output::Headphones);
      break;
    case MenuRow::Forget:
      if (!scanning) audiobook::forgetHeadphones();
      break;
    case MenuRow::Scan:
      if (halAudio.isBusy()) break;
      startScan();
      return;
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
    audiobook::scanForHeadphones();
  } else if (code >= 0 && static_cast<size_t>(code) < halAudio.deviceCount()) {
    // Saved with the output set to headphones once it connects.
    audiobook::pairWith(halAudio.device(code).address);
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
