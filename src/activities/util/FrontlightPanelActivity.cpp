#include "FrontlightPanelActivity.h"

#include <BlePageTurner.h>
#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <iterator>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/icons/customListIcons.h"
#include "components/icons/listIcons.h"
#include "components/icons/readerToolbarIcons.h"
#include "util/SlideSwitch.h"

#if CROSSPOINT_AUDIOBOOK
#include <BookModel.h>
#include <HalAudio.h>
#include <Logging.h>
#include <Memory.h>

#include "I18nKeys.h"
#include "RecentBooksStore.h"
#include "activities/ActivityManager.h"
#include "activities/settings/BtHeadphonesActivity.h"
#include "audio/AudioOutput.h"
#endif

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_BRIGHTNESS = 1;
constexpr fui::ActionId ACTION_WARMTH = 2;
constexpr fui::ActionId ACTION_TOGGLE = 3;
constexpr fui::ActionId ACTION_BRIGHTNESS_STEP = 4;
constexpr fui::ActionId ACTION_WARMTH_STEP = 5;
constexpr fui::ActionId ACTION_TILE = 6;  // value = tile index
#if CROSSPOINT_AUDIOBOOK
// Every audio control shares one handler, so the panel stays within the
// handler budget of UiAppHost::UiApp.
constexpr fui::ActionId ACTION_AUDIO = 7;  // value = AudioControl
enum class AudioControl : int16_t {
  OpenPlayer,
  ChapterSeek,  // slider; applied when the finger lifts
  PreviousChapter,
  Back30,
  PlayPause,
  Forward30,
  NextChapter,
  VolumeDown,
  VolumeSet,  // slider; the event's dragPermille is the level
  VolumeUp,
  Output,
};
constexpr int32_t SKIP_SECONDS = 30;
// Sizes of the audio section; computePanelBottom() mirrors them.
constexpr int16_t kCardHeight = 56;
constexpr int16_t kAudioRowHeight = 44;  // chapter and volume rows
constexpr AudioControls::TransportSizes kPanelTransport{48, 64, 72, 12};
constexpr int16_t kTimeLabelWidth = 76;
#endif

// iOS-style geometry. The panel is a card hanging from the top of the screen:
// a grabber, full-width slider pills, then a row of quick-setting icons. The chrome
// itself is fui::sheet / fui::sliderRow; these constants only
// size the bands, and computePanelBottom() mirrors them.
constexpr int16_t kPanelSideMargin = 16;
constexpr int16_t kGrabberHeight = 5;     // fui::SheetProps default, mirrored here
constexpr int16_t kSliderRowHeight = 56;  // the pill itself (finger-sized)
constexpr int16_t kQuickControlSize = kSliderRowHeight;
constexpr int16_t kQuickIconSize = 32;
// One percent per press, on the -/+ buttons and on the physical Left/Right keys
// alike (both repeat while held), so a level can be set exactly.
constexpr int BRIGHTNESS_STEP = 1;
// The dimmest setting is 1%, not 0: turning the light off is what the lamp
// button next to the slider is for, so a 0% "on" level would only be a second,
// worse way to reach the same place.
constexpr uint8_t MIN_BRIGHTNESS = 1;

uint8_t percentFromPermille(const int16_t permille) {
  int value = (static_cast<int>(permille) * 100 + 500) / 1000;
  if (value < 0) value = 0;
  if (value > 100) value = 100;
  return static_cast<uint8_t>(value);
}
}  // namespace

FrontlightPanelActivity::FrontlightPanelActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("FrontlightPanel", renderer, mappedInput), UiAppHost(renderer) {}

void FrontlightPanelActivity::onEnter() {
  Activity::onEnter();

  // A stored 0% predates the 1% floor (or came from the web settings): show it
  // as the floor rather than a level the slider can no longer produce. onExit
  // persists that, which is the intent — 0 is not a brightness any more.
  brightness = std::max(MIN_BRIGHTNESS, Frontlight.brightness());
  warmth = Frontlight.warmth();
  lightOn = Frontlight.isOn();
  lightOnChanged = false;

  // Seed the touch tile's restore mode from the live setting, so toggling off
  // and back on within this session returns to the mode the user had.
  if (SETTINGS.touchReaderControls != CrossPointSettings::TOUCH_READER_OFF) {
    touchModeRestore = SETTINGS.touchReaderControls;
  }

#if CROSSPOINT_AUDIOBOOK
  volume = SETTINGS.audio.volume;
  volumeChanged = false;
  findRecentAudiobook();
  refreshAudio();
#endif

  resetUi();
  app.on(ACTION_BRIGHTNESS, &FrontlightPanelActivity::onBrightnessEvent, this);
  app.on(ACTION_WARMTH, &FrontlightPanelActivity::onWarmthEvent, this);
  app.on(ACTION_TOGGLE, &FrontlightPanelActivity::onToggleEvent, this);
  app.on(ACTION_BRIGHTNESS_STEP, &FrontlightPanelActivity::onBrightnessStepEvent, this);
  app.on(ACTION_WARMTH_STEP, &FrontlightPanelActivity::onWarmthStepEvent, this);
  app.on(ACTION_TILE, &FrontlightPanelActivity::onTileEvent, this);
#if CROSSPOINT_AUDIOBOOK
  app.on(ACTION_AUDIO, &FrontlightPanelActivity::onAudioEvent, this);
  // A dropped handler would leave its controls silently dead.
  if (app.handlerOverflowed()) LOG_ERR("PANEL", "Too many UI action handlers");
#endif
  app.setScreen(&FrontlightPanelActivity::panelScreen, this);
  requestUpdate();
}

void FrontlightPanelActivity::persistLightSettings() {
  if (!Frontlight.present()) return;
  // brightness/warmth are always restored unconditionally on boot (see
  // main.cpp), so they never diverge from SETTINGS at onEnter() — comparing
  // against SETTINGS here only fires on a genuine user change. lightOn has
  // no such guarantee (see lightOnChanged's declaration), so it's gated on
  // the user actually having touched it this session instead.
  const bool changed = SETTINGS.frontlightBrightness != brightness || SETTINGS.frontlightWarmth != warmth ||
                       (lightOnChanged && SETTINGS.frontlightOn != (lightOn ? 1 : 0));
  if (changed) {
    SETTINGS.frontlightBrightness = brightness;
    SETTINGS.frontlightWarmth = warmth;
    if (lightOnChanged) SETTINGS.frontlightOn = lightOn ? 1 : 0;
    SETTINGS.saveToFile();
  }
}

void FrontlightPanelActivity::onExit() {
  persistLightSettings();
#if CROSSPOINT_AUDIOBOOK
  if (volumeChanged) SETTINGS.saveToFile();
#endif
  Activity::onExit();
}

void FrontlightPanelActivity::onBrightnessEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille < 0) return;
  self->brightness = std::max(MIN_BRIGHTNESS, percentFromPermille(event.dragPermille));
  Frontlight.setBrightness(self->brightness);
  if (!self->lightOn && !slideSwitchOwnsLight()) {
    self->lightOn = true;
    self->lightOnChanged = true;
    Frontlight.setOn(true);
  }
}

void FrontlightPanelActivity::onWarmthEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille < 0) return;
  self->warmth = percentFromPermille(event.dragPermille);
  Frontlight.setWarmth(self->warmth);
}

void FrontlightPanelActivity::onToggleEvent(const fui::ActionEvent&, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->toggleLight();
}

void FrontlightPanelActivity::onBrightnessStepEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->adjustBrightness(event.value);
}

void FrontlightPanelActivity::onWarmthStepEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->adjustWarmth(event.value);
}

void FrontlightPanelActivity::onTileEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->runTile(event.value);
}

#if CROSSPOINT_AUDIOBOOK
void FrontlightPanelActivity::onAudioEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->runAudioControl(event.value, event.dragPermille);
}

void FrontlightPanelActivity::findRecentAudiobook() {
  recentPath.clear();
  recentTitle.clear();
  for (const auto& book : RECENT_BOOKS.getBooks()) {
    // A book removed from the card stays in Recents until the Library prunes it.
    if (!audiobook::isAudiobookFile(book.path) || RecentBooksStore::isMissing(book)) continue;
    recentPath = book.path;
    recentTitle = book.title;
    return;
  }
}

void FrontlightPanelActivity::setVolume(const int value) {
  const uint8_t next = static_cast<uint8_t>(value < 0 ? 0 : value > 100 ? 100 : value);
  if (next == volume) return;
  volume = next;
  volumeChanged = true;
  // Live while a book plays; saved when the panel closes.
  if (AUDIOBOOK.isOpen()) {
    AUDIOBOOK.setVolume(volume);
  } else {
    SETTINGS.audio.volume = volume;
  }
}

void FrontlightPanelActivity::runAudioControl(const int control, const int16_t dragPermille) {
  switch (static_cast<AudioControl>(control)) {
    case AudioControl::OpenPlayer:
      app.clearTapFlash();
      // Replacing the screen stack closes the panel too. A book that was not
      // open opens paused: a "go there" control should not start sound.
      if (audioBookOpen) {
        activityManager.goToAudiobookPlayer(AUDIOBOOK.openedPath(), /*autoPlay=*/false);
      } else if (!recentPath.empty()) {
        activityManager.goToAudiobookPlayer(recentPath, /*autoPlay=*/false);
      } else {
        activityManager.goToFileBrowser("/");
      }
      return;
    case AudioControl::ChapterSeek: {
      // A finger held still sends the same position every frame.
      if (dragPermille < 0 || !chapterBar.moves(dragPermille)) return;
      bool changed = false;
      {
        RenderLock lock(*this);
        changed = chapterBar.drag(dragPermille);
      }
      if (changed) requestUpdate();
      return;
    }
    case AudioControl::PreviousChapter:
      AUDIOBOOK.previousChapter();
      break;
    case AudioControl::Back30:
      AUDIOBOOK.seekBy(-SKIP_SECONDS);
      break;
    case AudioControl::PlayPause:
      AUDIOBOOK.toggle();
      break;
    case AudioControl::Forward30:
      AUDIOBOOK.seekBy(SKIP_SECONDS);
      break;
    case AudioControl::NextChapter:
      AUDIOBOOK.nextChapter();
      break;
    case AudioControl::VolumeDown:
      setVolume(volume - audiobook::AudioSettings::VOLUME_STEP);
      requestUpdate();
      return;
    case AudioControl::VolumeSet:
      // Only a changed level repaints; see the audio-drag note in loop().
      if (dragPermille < 0 || percentFromPermille(dragPermille) == volume) return;
      setVolume(percentFromPermille(dragPermille));
      requestUpdate();
      return;
    case AudioControl::VolumeUp:
      setVolume(volume + audiobook::AudioSettings::VOLUME_STEP);
      requestUpdate();
      return;
    case AudioControl::Output:
      // As on the headphones screen: a scan holds the output until it ends.
      if (halAudio.link() == HalAudio::Link::Scanning) return;
      if (audiobook::currentOutput() == HalAudio::Output::Headphones) {
        audiobook::chooseOutput(HalAudio::Output::Speaker);
      } else if (SETTINGS.audio.headsetAddress[0] != '\0') {
        // A playing book pauses and resumes on the new output by itself.
        audiobook::chooseOutput(HalAudio::Output::Headphones);
      } else {
        // Nothing to connect to: pair first. The panel closes on the way back
        // so the screen under it repaints whole.
        app.clearTapFlash();
        auto next = makeUniqueNoThrow<BtHeadphonesActivity>(renderer, mappedInput);
        if (!next) {
          LOG_ERR("PANEL", "OOM: BtHeadphonesActivity");
          return;
        }
        startActivityForResult(std::move(next), [](const ActivityResult&) { finish(); });
        return;
      }
      break;
  }
  refreshAudio();
  requestUpdate();
}

bool FrontlightPanelActivity::refreshAudio(const bool endDrag) {
  RenderLock lock(*this);
  if (endDrag) {
    chapterBar.endDrag();
    // render() reads and clears this under the same lock.
    cleanRefreshPending = true;
  }
  const audiobook::PlayerSnapshot next = AUDIOBOOK.snapshot();
  const bool bookOpen = AUDIOBOOK.isOpen() && next.open;
  const bool headphones = audiobook::currentOutput() == HalAudio::Output::Headphones;
  char line[sizeof(cardLine)];
  std::string title;
  if (bookOpen) {
    title = AUDIOBOOK.title();
    char chapter[32];
    snprintf(chapter, sizeof(chapter), tr(STR_AUDIOBOOK_CHAPTER_POSITION), static_cast<unsigned>(next.chapter + 1),
             static_cast<unsigned>(next.chapterCount));
    snprintf(line, sizeof(line), "%s - %s", chapter, audiobook::playbackStateText(next));
  } else {
    // While nothing plays, the second line is free for the headphones' state.
    const char* output = audiobook::outputStatus();
    if (recentPath.empty()) {
      title = tr(STR_AUDIOBOOK_NO_AUDIOBOOK);
      snprintf(line, sizeof(line), "%s", output[0] != '\0' ? output : tr(STR_BROWSE_FILES));
    } else {
      title = recentTitle;
      snprintf(line, sizeof(line), "%s", output[0] != '\0' ? output : tr(STR_AUDIOBOOK_NOT_PLAYING));
    }
  }
  shownBookRevision = AUDIOBOOK.revision();
  shownOutputRevision = halAudio.revision();
  const bool changed = bookOpen != audioBookOpen || headphones != headphonesOut || title != cardTitle ||
                       strcmp(line, cardLine) != 0 || chapterBar.stale(next) ||
                       AudioControls::playState(next) != AudioControls::playState(shown) ||
                       next.chapterLengthMs != shown.chapterLengthMs;
  shown = next;
  chapterBar.show(next);
  audioBookOpen = bookOpen;
  headphonesOut = headphones;
  if (title != cardTitle) cardTitle = std::move(title);
  memcpy(cardLine, line, sizeof(cardLine));
  return changed;
}
#endif

void FrontlightPanelActivity::runTile(const int idx) {
  switch (idx) {
    case 0:  // Night mode (inverted output polarity, applied to the whole UI)
      SETTINGS.screenInverted = SETTINGS.screenInverted ? 0 : 1;
      SETTINGS.saveToFile();
      // Inversion rewrites every pixel; take the clean waveform so the panel
      // does not keep a ghost of the old polarity.
      cleanRefreshPending = true;
      requestUpdate();
      break;
    case 1:  // Ghost-cleanup refresh of the whole frame
      // Refreshing with the panel still up would clean a frame the user is
      // about to dismiss anyway: drop the panel first and let the repaint of
      // the screen underneath carry the clean waveform instead.
      renderer.promoteNextRefresh(HalDisplay::FULL_REFRESH);
      close();
      break;
    case 2:  // Cycle the reading orientation
      SETTINGS.orientation = static_cast<uint8_t>((SETTINGS.orientation + 1) % CrossPointSettings::ORIENTATION_COUNT);
      SETTINGS.saveToFile();
      // Only the setting changes: turning the renderer cropped the portrait-only
      // screens the panel opens over. The reader reflows on its next loop().
      requestUpdate();
      break;
    case 3:  // Touch reader controls (for reading with the palm on the glass)
      // Toggles the existing Settings -> Controls option, nothing lower-level:
      // that setting only governs the reader's tap/swipe handling, so the
      // panel's own gestures (including the swipe that reopens it) keep
      // working while it is off. Off remembers the mode (Tap/Swipe/Inverted
      // Tap) so toggling back does not stomp the user's choice.
      if (SETTINGS.touchReaderControls != CrossPointSettings::TOUCH_READER_OFF) {
        touchModeRestore = SETTINGS.touchReaderControls;
        SETTINGS.touchReaderControls = CrossPointSettings::TOUCH_READER_OFF;
      } else {
        SETTINGS.touchReaderControls = touchModeRestore;
      }
      SETTINGS.saveToFile();
      requestUpdate();
      break;
    case 4:  // Bluetooth page turner
      if (!bleturner::status().compiledIn) break;
      SETTINGS.ble.enabled = SETTINGS.ble.enabled ? 0 : 1;
      // The panel suspends the radio; tick() reconnects in an eligible screen.
      if (!SETTINGS.ble.enabled) bleturner::switchOff();
      SETTINGS.saveToFile();
      requestUpdate();
      break;
    default:
      break;
  }
}

void FrontlightPanelActivity::adjustBrightness(const int delta) {
  int next = static_cast<int>(brightness) + delta;
  if (next < MIN_BRIGHTNESS) next = MIN_BRIGHTNESS;
  if (next > 100) next = 100;
  if (next == brightness) return;
  brightness = static_cast<uint8_t>(next);
  Frontlight.setBrightness(brightness);
  if (!lightOn && !slideSwitchOwnsLight()) {
    lightOn = true;
    lightOnChanged = true;
    Frontlight.setOn(true);
  }
  requestUpdate();
}

void FrontlightPanelActivity::adjustWarmth(const int delta) {
  int next = static_cast<int>(warmth) + delta;
  if (next < 0) next = 0;
  if (next > 100) next = 100;
  if (next == warmth) return;
  warmth = static_cast<uint8_t>(next);
  Frontlight.setWarmth(warmth);
  requestUpdate();
}

void FrontlightPanelActivity::toggleLight() {
  lightOn = !lightOn;
  lightOnChanged = true;
  Frontlight.setOn(lightOn);
  requestUpdate();
}

void FrontlightPanelActivity::close() { finish(); }

bool FrontlightPanelActivity::handleHomeGesture() {
  close();
  return true;
}

void FrontlightPanelActivity::loop() {
  const auto touch = routeTouch(mappedInput, false, /*routeHeld=*/true);
  if (touch.routed) {
#if CROSSPOINT_AUDIOBOOK
    // The audio sliders repaint themselves only when what they show changes;
    // a finger resting on one would otherwise refresh the panel every frame.
    const bool audioDrag = touch.event.action == ACTION_AUDIO && touch.event.dragPermille >= 0;
    if (app.invalidated() && !audioDrag) requestUpdate();
#else
    if (app.invalidated()) requestUpdate();
#endif
    if (touch) {
      if (touch.event.dragPermille >= 0) draggingSlider = true;
      return;
    }
    // Swipe up dismisses the sheet, the way it was pulled down from the top
    // edge. draggingSlider keeps a fast slider flick from closing it.
    if (!draggingSlider && mappedInput.wasSwipe() == MappedInputManager::SwipeDir::Up) {
      close();
      return;
    }
    // panelBottom > 0 guards the frame the sheet opens in: the release that
    // opened it (a status-bar tap) is still in the input snapshot when the panel
    // runs its first loop(), and panelBottom is only known once render() has
    // measured the layout — so at 0 that release read as "tapped below the
    // sheet" and closed it again before it was ever drawn.
    if (touch.snap.touchReleased && !draggingSlider && panelBottom > 0 && touch.snap.touchY >= panelBottom) {
      close();
      return;
    }
  }
  if (draggingSlider) {
    if (!touch.snap.touchHeld) {
      draggingSlider = false;
#if CROSSPOINT_AUDIOBOOK
      // A chapter drag seeks once, when the finger lifts: while the drag still
      // shows its target and outside the lock (the seek writes the saved
      // position to the card); the drag then ends together with taking the
      // new position, so no frame shows the old one.
      size_t chapter = 0;
      uint64_t target = 0;
      if (chapterBar.pendingSeek(chapter, target)) AUDIOBOOK.seekInChapter(chapter, target);
      refreshAudio(/*endDrag=*/true);
#endif
      cleanRefreshPending = true;
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    close();
    return;
  }
#if CROSSPOINT_AUDIOBOOK
  // Up/Down are the volume keys on the Metalio.
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    setVolume(volume + audiobook::AudioSettings::VOLUME_STEP);
    requestUpdate();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    setVolume(volume - audiobook::AudioSettings::VOLUME_STEP);
    requestUpdate();
    return;
  }
  if (!Frontlight.present() && AUDIOBOOK.isOpen() && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    runAudioControl(static_cast<int>(AudioControl::PlayPause), -1);
    return;
  }
  // Repaint for a changed play state, output or shown minute, not for every
  // module reply.
  if ((AUDIOBOOK.revision() != shownBookRevision || halAudio.revision() != shownOutputRevision ||
       chapterBar.stale(AUDIOBOOK.snapshot())) &&
      refreshAudio()) {
    requestUpdate();
  }
#endif
  if (!Frontlight.present()) return;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!slideSwitchOwnsLight()) toggleLight();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left},
                                       [this] { adjustBrightness(-BRIGHTNESS_STEP); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right},
                                       [this] { adjustBrightness(BRIGHTNESS_STEP); });
}

int FrontlightPanelActivity::computePanelBottom() const {
  const auto tokens = uiThemeTokens(uiTarget);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int16_t lineHeight = uiTarget.lineHeight(tokens.smallText.font);
  // Slim battery band + the air around it (mirrors buildPanelScreen).
  const int y0 = std::max<int>(metrics.batteryHeight, lineHeight);
  int y = tokens.spaceMd + y0 + tokens.spaceMd;
  if (Frontlight.present()) {
    // Screen::sliderRow reserves caption + spaceMd + control band, then a
    // spaceMd gap; addSliderRow() adds one more spaceMd of air after each row.
    y += lineHeight + tokens.spaceMd + kSliderRowHeight + 2 * tokens.spaceMd;  // brightness
    if (Frontlight.hasColorTemperature()) {
      y += lineHeight + tokens.spaceMd + kSliderRowHeight + 2 * tokens.spaceMd;  // warmth
    }
    y += tokens.spaceSm;
  }
#if CROSSPOINT_AUDIOBOOK
  // Mirrors buildAudioSection(): card, then chapter and transport rows while
  // a book is open, then the volume row.
  y += kCardHeight + tokens.spaceMd;
  if (audioBookOpen) {
    y += kAudioRowHeight + tokens.spaceSm;
    y += kPanelTransport.play + tokens.spaceMd;
  }
  y += kAudioRowHeight + tokens.spaceMd;
#endif
  // Icons are touch targets, so a buttons-only board gets no row and the
  // sheet is exactly the frontlight controls.
  if (mappedInput.hasTouch()) y += kQuickControlSize;
  // The sheet's grabber band: content margin + grabber + air to the edge.
  // buildPanelScreen() feeds the same theme spacings into SheetProps.
  y += 2 * (tokens.spaceLg + tokens.spaceMd) + kGrabberHeight;
  return y;
}

void FrontlightPanelActivity::panelScreen(UiScreen& screen, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->buildPanelScreen(screen);
}

void FrontlightPanelActivity::addSliderRow(UiScreen& screen, const char* label, const uint8_t value,
                                           const fui::ActionId sliderAction, const fui::ActionId stepAction,
                                           const bool showToggle) {
  // Live percentage readout. The row draws before this call returns
  // (immediate mode), so borrowing a stack buffer is safe.
  char pct[8];
  snprintf(pct, sizeof(pct), "%u%%", static_cast<unsigned>(value));

  // rowProps is a member (fui::SliderRowProps embeds a 324-byte StyleSet, well
  // past the 256-byte budget a local gets — AGENTS.md). Every field that
  // varies between the two rows is reassigned here; the rest keep their
  // constructed defaults, which already match the panel's card language.
  rowProps.label = label;
  rowProps.value = pct;
  rowProps.sliderValue = value;
  rowProps.sliderAction = sliderAction;
  rowProps.decrement = stepAction;
  rowProps.increment = stepAction;
  rowProps.decrementValue = -BRIGHTNESS_STEP;
  rowProps.incrementValue = BRIGHTNESS_STEP;
  if (showToggle) {
    // Lamp on/off after the +: the sliders set the level, this kills the light
    // outright. Filled glyph = on, outline = off.
    rowProps.toggleAction = ACTION_TOGGLE;
    rowProps.toggleIcon = fui::bitmapFromIcon(lightOn ? icon_sun_filled_32 : icon_sun_32);
  } else {
    rowProps.toggleAction = fui::NO_ACTION;
    rowProps.toggleIcon = fui::BitmapRef{};
  }
  screen.sliderRow(rowProps, kSliderRowHeight);
  // The wrapper's own trailing gap is one spaceMd; double it so the rows
  // breathe — a control band this tall reads cramped at the list cadence.
  screen.spacer(screen.theme().spaceMd);
}

void FrontlightPanelActivity::buildPanelScreen(UiScreen& screen) {
  const auto& theme = screen.theme();

  // Sheet chrome first: the card body, the 2px rule along its bottom edge, and
  // the grabber on the edge the sheet is dragged from. Screen::sheet() also
  // clamps the content area to the sheet, so every band below lays out inside
  // it. (No header: the panel is a floating card, and its own grabber says
  // what it is.)
  fui::SheetProps sheetProps;
  // Keep equal padding above and below the grabber.
  sheetProps.grabberMargin = static_cast<int16_t>(theme.spaceLg + theme.spaceMd);
  sheetProps.grabberInset = sheetProps.grabberMargin;
  screen.sheet(sheetProps, static_cast<int16_t>(panelBottom));
  screen.insetContent(fui::Insets{0, kPanelSideMargin, 0, kPanelSideMargin});

  // Reuse the exact battery renderer and header rectangle used by Home. Call
  // the base implementation directly because RoundedRaff suppresses its
  // untitled Home header.
  {
    const auto& metrics = UITheme::getInstance().getMetrics();
    screen.spacer(theme.spaceMd);
    const int16_t bandH = std::max<int16_t>(static_cast<int16_t>(metrics.batteryHeight),
                                            screen.target().lineHeight(theme.smallText.font));
    screen.takeTop(bandH, theme.spaceMd);
    UITheme::getInstance().getTheme().BaseTheme::drawHeader(
        renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.homeTopPadding - metrics.topPadding},
        nullptr);
  }

  if (Frontlight.present()) {
    addSliderRow(screen, tr(STR_BRIGHTNESS), brightness, ACTION_BRIGHTNESS, ACTION_BRIGHTNESS_STEP,
                 /*showToggle=*/!slideSwitchOwnsLight());
    if (Frontlight.hasColorTemperature()) {
      addSliderRow(screen, tr(STR_WARMTH), warmth, ACTION_WARMTH, ACTION_WARMTH_STEP, /*showToggle=*/false);
    }
    screen.spacer(theme.spaceSm);
  }
#if CROSSPOINT_AUDIOBOOK
  buildAudioSection(screen);
#endif

  // Match the reader toolbar's equal icon slots; the full slot is the touch target.
  if (mappedInput.hasTouch()) {
    const int tileCount = bleturner::status().compiledIn ? kTileCount : kTileCount - 1;
    const fui::BitmapRef icons[kTileCount] = {
        fui::bitmapFromIcon(icon_moon_32), fui::bitmapFromIcon(icon_refresh_cw_32),
        fui::bitmapFromIcon(icon_reader_text_24), fui::bitmapFromIcon(icon_pointer_32),
        fui::bitmapFromIcon(SETTINGS.ble.enabled ? icon_bluetooth_32 : icon_bluetooth_off_32)};
    const bool enabled[kTileCount] = {SETTINGS.screenInverted != 0, false, false,
                                      SETTINGS.touchReaderControls != CrossPointSettings::TOUCH_READER_OFF,
                                      SETTINGS.ble.enabled != 0};
    static constexpr fui::Rotation rotations[] = {fui::Rotation::None, fui::Rotation::CCW90, fui::Rotation::R180,
                                                  fui::Rotation::CW90};
    // The underline rotates around the same 32px square as the toolbar's T.
    static constexpr fui::Rect underlines[] = {{4, 29, 24, 3}, {29, 4, 3, 24}, {4, 0, 24, 3}, {0, 4, 3, 24}};
    // The panel inherits the current screen rotation until the reader resumes.
    constexpr int orientationCount = CrossPointSettings::ORIENTATION_COUNT;
    const int preview = (SETTINGS.orientation + orientationCount - renderer.getOrientation()) % orientationCount;
    const fui::Rect row = screen.takeTop(kQuickControlSize);
    const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
    for (int id = 0; id < tileCount; ++id) {
      const int16_t left = static_cast<int16_t>(row.x + row.width * id / tileCount);
      const int16_t right = static_cast<int16_t>(row.x + row.width * (id + 1) / tileCount);
      const fui::Rect slot{left, row.y, static_cast<int16_t>(right - left), row.height};
      const fui::Rect control{static_cast<int16_t>(slot.x + (slot.width - kQuickControlSize) / 2), row.y,
                              kQuickControlSize, kQuickControlSize};
      if (enabled[id]) {
        screen.target().fill(control, ink, theme.controlRadius);
      } else {
        screen.target().stroke(control, ink, 2, theme.controlRadius);
      }
      const fui::Rect iconRect{static_cast<int16_t>(control.x + (control.width - kQuickIconSize) / 2),
                               static_cast<int16_t>(control.y + (control.height - kQuickIconSize) / 2), kQuickIconSize,
                               kQuickIconSize};
      const fui::Paint foreground = fui::Paint::solid(enabled[id] ? fui::Color::White : fui::Color::Black);
      screen.target().bitmap(iconRect, icons[id], fui::BitmapMode::Center, foreground,
                             id == 2 ? rotations[preview] : fui::Rotation::None);
      if (id == 2) {
        const auto& line = underlines[preview];
        screen.target().fill(fui::Rect{static_cast<int16_t>(iconRect.x + line.x),
                                       static_cast<int16_t>(iconRect.y + line.y), line.width, line.height},
                             ink);
      }
      // A tile whose function the slide switch owns shows its state but takes no tap.
      const bool switchOwned = (id == 0 && slideSwitchOwns(CrossPointSettings::SLIDE_SWITCH_NIGHT_MODE)) ||
                               (id == 3 && slideSwitchOwns(CrossPointSettings::SLIDE_SWITCH_TOUCH_CONTROLS));
      if (!switchOwned) screen.frame().hit(slot, ACTION_TILE, static_cast<int16_t>(id), fui::InputTouch);
    }
  }
}

#if CROSSPOINT_AUDIOBOOK
void FrontlightPanelActivity::buildCard(UiScreen& screen) {
  // [book] Title                         >
  //        Chapter 3 / 12 - Playing
  // One touch target: it opens the player (or the file browser).
  const auto& theme = screen.theme();
  const fui::Rect card = screen.takeTop(kCardHeight, theme.spaceMd);
  const int16_t value = static_cast<int16_t>(AudioControl::OpenPlayer);
  const fui::State state = screen.frame().stateFor(ACTION_AUDIO, value, fui::StateNormal);
  const bool pressed = fui::hasState(state, fui::StateActive);
  const fui::Paint ink = fui::Paint::solid(pressed ? fui::Color::White : fui::Color::Black);
  if (pressed) {
    screen.target().fill(card, fui::Paint::solid(fui::Color::Black), theme.controlRadius);
  } else {
    screen.target().stroke(card, ink, 2, theme.controlRadius);
  }

  constexpr int16_t kIcon = 32;
  constexpr int16_t kChevron = 24;
  const int16_t pad = theme.spaceMd + theme.spaceXs;
  const bool hasBook = audioBookOpen || !recentPath.empty();
  const fui::Rect iconRect{static_cast<int16_t>(card.x + pad), static_cast<int16_t>(card.y + (card.height - kIcon) / 2),
                           kIcon, kIcon};
  screen.target().bitmap(
      iconRect,
      hasBook ? AudioControls::icon(AudioControls::Icon::Audiobook, kIcon) : fui::bitmapFromIcon(icon_folder_32),
      fui::BitmapMode::Center, ink);
  const fui::Rect chevron{static_cast<int16_t>(card.right() - pad - kChevron),
                          static_cast<int16_t>(card.y + (card.height - kChevron) / 2), kChevron, kChevron};
  screen.target().bitmap(chevron, fui::bitmapFromIcon(icon_reader_next_24), fui::BitmapMode::Center, ink);

  auto style = theme.smallText;
  style.maxLines = 1;
  style.color = pressed ? fui::Color::White : fui::Color::Black;
  const int16_t lineHeight = screen.target().lineHeight(style.font);
  const int16_t textX = static_cast<int16_t>(iconRect.right() + pad);
  const int16_t textW = static_cast<int16_t>(chevron.x - theme.spaceSm - textX);
  const int16_t top = static_cast<int16_t>(card.y + (card.height - 2 * lineHeight) / 2);
  style.bold = true;
  screen.target().text(fui::Rect{textX, top, textW, lineHeight}, cardTitle.c_str(), style);
  style.bold = false;
  screen.target().text(fui::Rect{textX, static_cast<int16_t>(top + lineHeight), textW, lineHeight}, cardLine, style);
  screen.frame().hit(card, ACTION_AUDIO, value, fui::InputTouch);
}

void FrontlightPanelActivity::buildAudioSection(UiScreen& screen) {
  const auto& theme = screen.theme();
  const auto value = [](const AudioControl control) { return static_cast<int16_t>(control); };
  buildCard(screen);

  auto labelStyle = theme.smallText;
  labelStyle.maxLines = 1;
  if (audioBookOpen) {
    // 12 min ======o---------- -23 min
    const fui::Rect row = screen.takeTop(kAudioRowHeight, theme.spaceSm);
    const int16_t gap = theme.spaceSm;
    const fui::Rect slider{static_cast<int16_t>(row.x + kTimeLabelWidth + gap), row.y,
                           static_cast<int16_t>(row.width - 2 * (kTimeLabelWidth + gap)), row.height};
    chapterBar.slider(screen, slider, ACTION_AUDIO, value(AudioControl::ChapterSeek));
    const int16_t lineHeight = screen.target().lineHeight(labelStyle.font);
    const int16_t labelY = static_cast<int16_t>(row.y + (row.height - lineHeight) / 2);
    labelStyle.bold = chapterBar.dragging();
    screen.target().text(fui::Rect{row.x, labelY, kTimeLabelWidth, lineHeight}, chapterBar.elapsedLabel(), labelStyle);
    labelStyle.align = fui::TextAlign::Right;
    screen.target().text(
        fui::Rect{static_cast<int16_t>(row.right() - kTimeLabelWidth), labelY, kTimeLabelWidth, lineHeight},
        chapterBar.leftLabel(), labelStyle);

    audioControls.transportRow(
        screen, screen.takeTop(kPanelTransport.play, theme.spaceMd), kPanelTransport, ACTION_AUDIO,
        {value(AudioControl::PreviousChapter), value(AudioControl::Back30), value(AudioControl::PlayPause),
         value(AudioControl::Forward30), value(AudioControl::NextChapter)},
        AudioControls::playState(shown));
  }

  // [vol-] ======o------ [vol+] [output]
  const fui::Rect row = screen.takeTop(kAudioRowHeight, theme.spaceMd);
  const int16_t size = row.height;
  const int16_t gap = theme.spaceMd;
  const fui::Rect output{static_cast<int16_t>(row.right() - size), row.y, size, size};
  const fui::Rect up{static_cast<int16_t>(output.x - gap - size), row.y, size, size};
  const fui::Rect down{row.x, row.y, size, size};
  const fui::Rect slider{static_cast<int16_t>(down.right() + gap), row.y,
                         static_cast<int16_t>(up.x - gap - down.right() - gap), size};
  audioControls.iconButton(screen, down, AudioControls::Icon::VolumeLow, ACTION_AUDIO, value(AudioControl::VolumeDown));
  AudioControls::slimSlider(screen, slider, volume, 100, ACTION_AUDIO, value(AudioControl::VolumeSet));
  audioControls.iconButton(screen, up, AudioControls::Icon::VolumeHigh, ACTION_AUDIO, value(AudioControl::VolumeUp));
  audioControls.iconButton(screen, output,
                           headphonesOut ? AudioControls::Icon::Headphones : AudioControls::Icon::Speaker, ACTION_AUDIO,
                           value(AudioControl::Output));
}
#endif

void FrontlightPanelActivity::render(RenderLock&&) {
  panelBottom = computePanelBottom();

  // fui::sheet draws the card body, its bottom rule, and the grabber during
  // renderUi(); the battery band at the card's top is part of the screen build.
  renderUi();

  // A tile that rewrote the whole frame (night mode) re-drives every pixel
  // once; ordinary repaints stay on the fast path. HALF: strong enough to
  // flip the whole frame's polarity without the FULL waveform's blackout
  // flash. Any faint residue clears with the panel's dedicated refresh tile
  // or the next scheduled clean refresh.
  renderer.displayBuffer(cleanRefreshPending ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  cleanRefreshPending = false;
}
