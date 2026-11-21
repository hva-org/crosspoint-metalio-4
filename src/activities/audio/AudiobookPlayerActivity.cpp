#include "AudiobookPlayerActivity.h"

#if CROSSPOINT_AUDIOBOOK

#include <GfxRenderer.h>
#include <HalAudio.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <utility>

#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "activities/audio/AudiobookChapterListActivity.h"
#include "activities/settings/BtHeadphonesActivity.h"
#include "audio/AudioOutput.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_CONTROL = 1;
constexpr int32_t SKIP_SECONDS = 30;

// Portrait and landscape button sizes; see AudioControls::TransportSizes.
constexpr AudioControls::TransportSizes TALL_TRANSPORT{56, 72, 104, 18};
constexpr AudioControls::TransportSizes WIDE_TRANSPORT{48, 64, 88, 10};
constexpr int16_t SLIDER_HEIGHT = 48;
constexpr int16_t VOLUME_ROW_HEIGHT = 44;
constexpr int16_t TALL_BUTTON = 56;
constexpr int16_t WIDE_BUTTON = 48;
constexpr int16_t VOLUME_LABEL_WIDTH = 56;
constexpr int16_t SPEED_LABEL_WIDTH = 80;
}  // namespace

void AudiobookPlayerActivity::onEnter() {
  Activity::onEnter();
  resetUi();
  app.on(ACTION_CONTROL, &AudiobookPlayerActivity::onControl, this);
  app.setScreen(&AudiobookPlayerActivity::screenFn, this);

  // Opening scans the folder, parses chapters and starts the audio module
  // (about a second); show the loading line first. The open book needs none
  // of that, so it goes straight to the player.
  const bool alreadyOpen = AUDIOBOOK.owns(path);
  if (!alreadyOpen) {
    {
      RenderLock lock(*this);
      snprintf(statusLine, sizeof(statusLine), "%s", tr(STR_LOADING));
    }
    requestUpdateAndWait();
  }
  if (!AUDIOBOOK.open(path)) {
    LOG_ERR("ABK", "Cannot open %s", path.c_str());
    openFailed = true;
  } else if (!alreadyOpen && autoPlay) {
    AUDIOBOOK.play();
  }
  refresh();
  requestUpdate();
}

void AudiobookPlayerActivity::refresh(const bool endDrag) {
  RenderLock lock(*this);
  if (endDrag) {
    chapterBar.endDrag();
    // render() reads and clears this under the same lock.
    cleanRefreshPending = true;
  }
  shown = AUDIOBOOK.snapshot();
  shownRevision = AUDIOBOOK.revision();
  chapterBar.show(shown);
  if (openFailed || !shown.open) {
    snprintf(statusLine, sizeof(statusLine), "%s", tr(STR_AUDIOBOOK_OPEN_FAILED));
    return;
  }
  title = AUDIOBOOK.title();
  const auto& titles = AUDIOBOOK.chapterTitles();
  chapterTitle = shown.chapter < titles.size() ? titles[shown.chapter].text : "";
  snprintf(chapterLine, sizeof(chapterLine), tr(STR_AUDIOBOOK_CHAPTER_POSITION),
           static_cast<unsigned>(shown.chapter + 1), static_cast<unsigned>(shown.chapterCount));

  // Playing and paused show in the play button; text only for what it cannot say.
  const bool noteworthy = shown.problem != audiobook::PlaybackProblem::None || shown.waitingForOutput || shown.finished;
  snprintf(statusLine, sizeof(statusLine), "%s", noteworthy ? audiobook::playbackStateText(shown) : "");
  snprintf(speedLabel, sizeof(speedLabel), "%gx", static_cast<double>(audiobook::playbackSpeed(shown.speedIndex)));
  snprintf(volumeLabel, sizeof(volumeLabel), "%u%%", static_cast<unsigned>(shown.volume));
}

void AudiobookPlayerActivity::changeVolume(const int delta) {
  const int volume = static_cast<int>(AUDIOBOOK.snapshot().volume) + delta;
  AUDIOBOOK.setVolume(static_cast<uint8_t>(volume < 0 ? 0 : volume > 100 ? 100 : volume));
}

void AudiobookPlayerActivity::handle(const Control control, const int16_t dragPermille) {
  switch (control) {
    case Control::Back30:
      AUDIOBOOK.seekBy(-SKIP_SECONDS);
      break;
    case Control::PlayPause:
      AUDIOBOOK.toggle();
      break;
    case Control::Forward30:
      AUDIOBOOK.seekBy(SKIP_SECONDS);
      break;
    case Control::PreviousChapter:
      AUDIOBOOK.previousChapter();
      break;
    case Control::NextChapter:
      AUDIOBOOK.nextChapter();
      break;
    case Control::SpeedDown:
    case Control::SpeedUp:
      AUDIOBOOK.setSpeed(audiobook::steppedSpeedIndex(shown.speedIndex, control == Control::SpeedUp ? 1 : -1));
      break;
    case Control::Chapters:
    case Control::Output: {
      app.clearTapFlash();
      std::unique_ptr<Activity> next;
      if (control == Control::Chapters) {
        next = makeUniqueNoThrow<AudiobookChapterListActivity>(renderer, mappedInput);
      } else {
        next = makeUniqueNoThrow<BtHeadphonesActivity>(renderer, mappedInput);
      }
      if (!next) {
        LOG_ERR("ABK", "OOM: audiobook sub-screen");
        return;
      }
      startActivityForResult(std::move(next), [this](const ActivityResult&) {
        refresh();
        requestUpdate();
      });
      return;
    }
    case Control::VolumeDown:
      changeVolume(-audiobook::AudioSettings::VOLUME_STEP);
      break;
    case Control::VolumeUp:
      changeVolume(audiobook::AudioSettings::VOLUME_STEP);
      break;
    case Control::VolumeSet: {
      if (dragPermille < 0) return;
      sliderTouched = true;
      const int volume = (dragPermille * 100 + 500) / 1000;
      if (volume == shown.volume) return;
      AUDIOBOOK.setVolume(static_cast<uint8_t>(volume));
      break;
    }
    case Control::ChapterSeek: {
      if (dragPermille < 0) return;
      sliderTouched = true;
      // A finger held still sends the same position every frame.
      if (!chapterBar.moves(dragPermille)) return;
      bool changed = false;
      {
        RenderLock lock(*this);
        changed = chapterBar.drag(dragPermille);
      }
      if (changed) requestUpdate();
      return;
    }
  }
  refresh();
  requestUpdate();
}

void AudiobookPlayerActivity::onControl(const fui::ActionEvent& event, void* user) {
  static_cast<AudiobookPlayerActivity*>(user)->handle(static_cast<Control>(event.value), event.dragPermille);
}

void AudiobookPlayerActivity::loop() {
  // Held frames too: the sliders follow the finger.
  const auto touch = routeTouch(mappedInput, false, /*routeHeld=*/true);
  // Slider events ask for a repaint themselves, only when what they show
  // changes; a finger held still on one would otherwise repaint every frame.
  if (touch.routed && app.invalidated() && touch.event.dragPermille < 0) requestUpdate();
  if (sliderTouched && !touch.snap.touchHeld) {
    // The finger lifted: a chapter drag seeks once, then one cleaning refresh.
    sliderTouched = false;
    // Seek while the drag still shows its target, outside the lock (the seek
    // writes the saved position to the card); then end the drag together
    // with taking the new position, so no frame shows the old one.
    size_t chapter = 0;
    uint64_t target = 0;
    if (chapterBar.pendingSeek(chapter, target)) AUDIOBOOK.seekInChapter(chapter, target);
    refresh(/*endDrag=*/true);
    requestUpdate();
    return;
  }
  if (touch) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (openFailed) return;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    handle(Control::PlayPause, -1);
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    handle(Control::VolumeUp, -1);
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    handle(Control::VolumeDown, -1);
  }

  // E-ink: repaint on state changes and when a shown minute moves; not while
  // a drag owns the screen.
  if (sliderTouched) return;
  if (AUDIOBOOK.revision() != shownRevision || chapterBar.stale(AUDIOBOOK.snapshot())) {
    refresh();
    requestUpdate();
  }
}

void AudiobookPlayerActivity::screenFn(UiScreen& screen, void* user) {
  static_cast<AudiobookPlayerActivity*>(user)->buildScreen(screen);
}

void AudiobookPlayerActivity::drawChapterBar(UiScreen& screen, const fui::Rect slider, const fui::Rect labels) {
  chapterBar.slider(screen, slider, ACTION_CONTROL, static_cast<int16_t>(Control::ChapterSeek));
  auto style = screen.theme().smallText;
  style.maxLines = 1;
  style.bold = chapterBar.dragging();
  screen.target().text(labels, chapterBar.elapsedLabel(), style);
  style.align = fui::TextAlign::Right;
  screen.target().text(labels, chapterBar.leftLabel(), style);
}

void AudiobookPlayerActivity::extrasRow(UiScreen& screen, const fui::Rect band) {
  // [-] 1.25x [+]   [chapters]   [output]
  const int16_t size = band.height;
  const bool headphones = halAudio.output() == HalAudio::Output::Headphones;
  const fui::Rect minus{band.x, band.y, size, size};
  const fui::Rect label{static_cast<int16_t>(minus.right()), band.y, SPEED_LABEL_WIDTH, size};
  const fui::Rect plus{static_cast<int16_t>(label.right()), band.y, size, size};
  const fui::Rect output{static_cast<int16_t>(band.right() - size), band.y, size, size};
  const fui::Rect chapters{static_cast<int16_t>((plus.right() + output.x - size) / 2), band.y, size, size};

  controls.iconButton(screen, minus, AudioControls::Icon::Minus, ACTION_CONTROL,
                      static_cast<int16_t>(Control::SpeedDown), shown.speedIndex > 0);
  auto style = screen.theme().bodyText;
  style.bold = true;
  style.align = fui::TextAlign::Center;
  style.maxLines = 1;
  const int16_t lineHeight = screen.target().lineHeight(style.font);
  screen.target().text(
      fui::Rect{label.x, static_cast<int16_t>(label.y + (size - lineHeight) / 2), label.width, lineHeight}, speedLabel,
      style);
  controls.iconButton(screen, plus, AudioControls::Icon::Plus, ACTION_CONTROL, static_cast<int16_t>(Control::SpeedUp),
                      shown.speedIndex + 1 < audiobook::PLAYBACK_SPEED_COUNT);
  controls.iconButton(screen, chapters, AudioControls::Icon::Chapters, ACTION_CONTROL,
                      static_cast<int16_t>(Control::Chapters));
  controls.iconButton(screen, output, headphones ? AudioControls::Icon::Headphones : AudioControls::Icon::Speaker,
                      ACTION_CONTROL, static_cast<int16_t>(Control::Output));
}

void AudiobookPlayerActivity::volumeRow(UiScreen& screen, const fui::Rect band) {
  // [vol-] ======o------ [vol+] 60%
  const int16_t size = band.height;
  const int16_t gap = screen.theme().spaceMd;
  const fui::Rect down{band.x, band.y, size, size};
  const fui::Rect label{static_cast<int16_t>(band.right() - VOLUME_LABEL_WIDTH), band.y, VOLUME_LABEL_WIDTH, size};
  const fui::Rect up{static_cast<int16_t>(label.x - size), band.y, size, size};
  const fui::Rect slider{static_cast<int16_t>(down.right() + gap), band.y,
                         static_cast<int16_t>(up.x - gap - down.right() - gap), size};

  controls.iconButton(screen, down, AudioControls::Icon::VolumeLow, ACTION_CONTROL,
                      static_cast<int16_t>(Control::VolumeDown));
  AudioControls::slimSlider(screen, slider, shown.volume, 100, ACTION_CONTROL,
                            static_cast<int16_t>(Control::VolumeSet));
  controls.iconButton(screen, up, AudioControls::Icon::VolumeHigh, ACTION_CONTROL,
                      static_cast<int16_t>(Control::VolumeUp));
  auto style = screen.theme().smallText;
  style.align = fui::TextAlign::Right;
  style.maxLines = 1;
  const int16_t lineHeight = screen.target().lineHeight(style.font);
  screen.target().text(
      fui::Rect{label.x, static_cast<int16_t>(label.y + (size - lineHeight) / 2), label.width, lineHeight}, volumeLabel,
      style);
}

void AudiobookPlayerActivity::buildTall(UiScreen& screen) {
  const auto& theme = screen.theme();
  const int16_t lineHeight = screen.target().lineHeight(theme.smallText.font);
  // Laid out from the bottom up, so the controls stay in place whatever the
  // chapter title's length.
  screen.spacer(theme.spaceMd, fui::LayoutAnchor::Bottom);
  volumeRow(screen, screen.takeBottom(VOLUME_ROW_HEIGHT, theme.spaceLg));
  extrasRow(screen, screen.takeBottom(TALL_BUTTON, theme.spaceLg));
  const fui::Rect status = screen.takeBottom(lineHeight, theme.spaceSm);
  const fui::Rect transport = screen.takeBottom(TALL_TRANSPORT.play, theme.spaceLg);
  const fui::Rect labels = screen.takeBottom(lineHeight, theme.spaceXs);
  const fui::Rect slider = screen.takeBottom(SLIDER_HEIGHT);

  drawChapterBar(screen, slider, labels);
  controls.transportRow(screen, transport, TALL_TRANSPORT, ACTION_CONTROL,
                        {static_cast<int16_t>(Control::PreviousChapter), static_cast<int16_t>(Control::Back30),
                         static_cast<int16_t>(Control::PlayPause), static_cast<int16_t>(Control::Forward30),
                         static_cast<int16_t>(Control::NextChapter)},
                        AudioControls::playState(shown));
  auto style = theme.smallText;
  style.align = fui::TextAlign::Center;
  style.maxLines = 1;
  if (statusLine[0] != '\0') screen.target().text(status, statusLine, style);
}

void AudiobookPlayerActivity::buildWide(UiScreen& screen) {
  const auto& theme = screen.theme();
  const int16_t lineHeight = screen.target().lineHeight(theme.smallText.font);
  // The chapter slider spans the width; the controls split into two columns
  // under it: transport on the left, speed, chapters, output and volume on
  // the right.
  const fui::Rect slider = screen.takeTop(SLIDER_HEIGHT);
  const fui::Rect labels = screen.takeTop(lineHeight, theme.spaceLg);
  drawChapterBar(screen, slider, labels);

  const fui::Rect rest = screen.body();
  const int16_t gutter = theme.spaceLg * 2;
  const int16_t columnWidth = static_cast<int16_t>((rest.width - gutter) / 2);
  const fui::Rect left{rest.x, rest.y, columnWidth, WIDE_TRANSPORT.play};
  controls.transportRow(screen, left, WIDE_TRANSPORT, ACTION_CONTROL,
                        {static_cast<int16_t>(Control::PreviousChapter), static_cast<int16_t>(Control::Back30),
                         static_cast<int16_t>(Control::PlayPause), static_cast<int16_t>(Control::Forward30),
                         static_cast<int16_t>(Control::NextChapter)},
                        AudioControls::playState(shown));
  auto style = theme.smallText;
  style.align = fui::TextAlign::Center;
  style.maxLines = 1;
  if (statusLine[0] != '\0') {
    screen.target().text(
        fui::Rect{rest.x, static_cast<int16_t>(left.bottom() + theme.spaceSm), columnWidth, lineHeight}, statusLine,
        style);
  }

  const int16_t rightX = static_cast<int16_t>(rest.x + columnWidth + gutter);
  extrasRow(screen, fui::Rect{rightX, rest.y, columnWidth, WIDE_BUTTON});
  volumeRow(screen, fui::Rect{rightX, static_cast<int16_t>(rest.y + WIDE_BUTTON + theme.spaceMd), columnWidth,
                              VOLUME_ROW_HEIGHT});
}

void AudiobookPlayerActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& theme = screen.theme();
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), static_cast<int16_t>(metrics.contentSidePadding),
      static_cast<int16_t>(metrics.buttonHintsHeight), static_cast<int16_t>(metrics.contentSidePadding)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  const int16_t width = screen.body().width;
  const bool wide = screen.body().width > screen.body().height;

  const auto line = [&screen, width](const char* text, fui::TextStyle style, const int16_t gapAfter) {
    if (text == nullptr || text[0] == '\0') return;
    const int16_t h = fui::measureWrappedText(screen.target(), text, style, width).height;
    screen.target().text(screen.takeTop(h, gapAfter), text, style);
  };
  auto titleStyle = theme.bodyText;
  titleStyle.align = fui::TextAlign::Center;
  titleStyle.bold = true;
  titleStyle.maxLines = wide ? 1 : 2;
  auto infoStyle = theme.smallText;
  infoStyle.align = fui::TextAlign::Center;
  infoStyle.maxLines = 1;

  if (openFailed || !shown.open) {
    titleStyle.maxLines = 2;
    line(statusLine, titleStyle, theme.spaceMd);
    return;
  }
  line(chapterTitle.c_str(), titleStyle, theme.spaceSm);
  line(chapterLine, infoStyle, theme.spaceLg);
  if (wide) {
    buildWide(screen);
  } else {
    buildTall(screen);
  }
}

void AudiobookPlayerActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const char* header = title.empty() ? tr(STR_LOADING) : title.c_str();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, header);
  renderUi();
  const auto labels = mappedInput.mapLabels(
      tr(STR_BACK), shown.playing || shown.waitingForOutput ? tr(STR_AUDIOBOOK_PAUSE) : tr(STR_AUDIOBOOK_PLAY),
      tr(STR_AUDIOBOOK_VOLUME_UP), tr(STR_AUDIOBOOK_VOLUME_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // A finished slider drag leaves ghosting the fast waveform cannot clear.
  renderer.displayBuffer(cleanRefreshPending ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  cleanRefreshPending = false;
}

#endif
