#include "AudiobookPlayerActivity.h"

#if CROSSPOINT_AUDIOBOOK

#include <GfxRenderer.h>
#include <HalAudio.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <initializer_list>
#include <utility>

#include "CrossPointSettings.h"
#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "activities/audio/AudiobookChapterListActivity.h"
#include "activities/settings/BtHeadphonesActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_CONTROL = 1;
constexpr int32_t SKIP_SECONDS = 30;
constexpr uint64_t MINUTE_MS = 60000;
}  // namespace

void AudiobookPlayerActivity::onEnter() {
  Activity::onEnter();
  resetUi();
  app.on(ACTION_CONTROL, &AudiobookPlayerActivity::onControl, this);
  app.setScreen(&AudiobookPlayerActivity::screenFn, this);

  // Opening scans the folder, parses chapters and starts the audio module
  // (about a second); show the loading line first.
  {
    RenderLock lock(*this);
    snprintf(statusLine, sizeof(statusLine), "%s", tr(STR_LOADING));
  }
  requestUpdateAndWait();
  const bool alreadyOpen = AUDIOBOOK.owns(path);
  if (!AUDIOBOOK.open(path)) {
    LOG_ERR("ABK", "Cannot open %s", path.c_str());
    openFailed = true;
  } else if (!alreadyOpen) {
    AUDIOBOOK.play();
  }
  refresh();
  requestUpdate();
}

void AudiobookPlayerActivity::refresh() {
  RenderLock lock(*this);
  shown = AUDIOBOOK.snapshot();
  shownRevision = AUDIOBOOK.revision();
  shownMinute = shown.chapterElapsedMs / MINUTE_MS;
  if (openFailed || !shown.open) {
    snprintf(statusLine, sizeof(statusLine), "%s", tr(STR_AUDIOBOOK_OPEN_FAILED));
    return;
  }
  title = AUDIOBOOK.title();
  const auto& titles = AUDIOBOOK.chapterTitles();
  chapterTitle = shown.chapter < titles.size() ? titles[shown.chapter].text : "";
  snprintf(chapterLine, sizeof(chapterLine), tr(STR_AUDIOBOOK_CHAPTER_POSITION),
           static_cast<unsigned>(shown.chapter + 1), static_cast<unsigned>(shown.chapterCount));

  // Whole minutes: the e-ink screen repaints once a minute while playing.
  const auto elapsedMin = static_cast<unsigned>(shown.chapterElapsedMs / MINUTE_MS);
  if (shown.chapterLengthMs > 0) {
    snprintf(timeLine, sizeof(timeLine), tr(STR_AUDIOBOOK_MINUTES_OF), elapsedMin,
             static_cast<unsigned>((shown.chapterLengthMs + MINUTE_MS - 1) / MINUTE_MS));
  } else {
    snprintf(timeLine, sizeof(timeLine), tr(STR_AUDIOBOOK_MINUTES), elapsedMin);
  }

  using audiobook::PlaybackProblem;
  const bool headphones = halAudio.output() == HalAudio::Output::Headphones;
  const char* waiting = headphones ? tr(STR_AUDIOBOOK_CONNECTING_HEADPHONES) : tr(STR_AUDIOBOOK_SWITCHING);
  const char* state = shown.problem == PlaybackProblem::Track    ? tr(STR_AUDIOBOOK_TRACK_FAILED)
                      : shown.problem == PlaybackProblem::Output ? tr(STR_AUDIOBOOK_NO_OUTPUT)
                      : shown.waitingForOutput                   ? waiting
                      : shown.finished                           ? tr(STR_AUDIOBOOK_FINISHED)
                      : shown.playing                            ? tr(STR_AUDIOBOOK_PLAYING)
                                                                 : tr(STR_AUDIOBOOK_PAUSED);
  const char* output = !headphones                             ? tr(STR_AUDIOBOOK_SPEAKER)
                       : SETTINGS.audio.headsetName[0] != '\0' ? SETTINGS.audio.headsetName
                                                               : tr(STR_AUDIOBOOK_HEADPHONES);
  snprintf(statusLine, sizeof(statusLine), "%s - %s", state, output);
  snprintf(speedLabel, sizeof(speedLabel), tr(STR_AUDIOBOOK_SPEED_VALUE),
           static_cast<double>(audiobook::playbackSpeed(shown.speedIndex)));
  snprintf(volumeLabel, sizeof(volumeLabel), tr(STR_AUDIOBOOK_VOLUME_VALUE), static_cast<unsigned>(shown.volume));
}

void AudiobookPlayerActivity::changeVolume(const int delta) {
  const int volume = static_cast<int>(AUDIOBOOK.snapshot().volume) + delta;
  AUDIOBOOK.setVolume(static_cast<uint8_t>(volume < 0 ? 0 : volume > 100 ? 100 : volume));
}

void AudiobookPlayerActivity::handle(const Control control) {
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
    case Control::Speed:
      AUDIOBOOK.setSpeed(static_cast<uint8_t>((shown.speedIndex + 1) % audiobook::PLAYBACK_SPEED_COUNT));
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
  }
  refresh();
  requestUpdate();
}

void AudiobookPlayerActivity::onControl(const fui::ActionEvent& event, void* user) {
  static_cast<AudiobookPlayerActivity*>(user)->handle(static_cast<Control>(event.value));
}

void AudiobookPlayerActivity::loop() {
  const auto touch = routeTouch(mappedInput);
  if (touch.routed && app.invalidated()) requestUpdate();
  if (touch) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (openFailed) return;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    handle(Control::PlayPause);
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    handle(Control::VolumeUp);
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    handle(Control::VolumeDown);
  }

  // E-ink: repaint on state changes and once a minute while time moves.
  const auto now = AUDIOBOOK.snapshot();
  if (AUDIOBOOK.revision() != shownRevision || now.chapterElapsedMs / MINUTE_MS != shownMinute) {
    refresh();
    requestUpdate();
  }
}

void AudiobookPlayerActivity::screenFn(UiScreen& screen, void* user) {
  static_cast<AudiobookPlayerActivity*>(user)->buildScreen(screen);
}

void AudiobookPlayerActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& theme = screen.theme();
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), static_cast<int16_t>(metrics.contentSidePadding),
      static_cast<int16_t>(metrics.buttonHintsHeight), static_cast<int16_t>(metrics.contentSidePadding)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  const int16_t width = screen.body().width;

  const auto line = [&screen, width](const char* text, fui::TextStyle style, const int16_t gapAfter) {
    if (text == nullptr || text[0] == '\0') return;
    const int16_t h = fui::measureWrappedText(screen.target(), text, style, width).height;
    screen.target().text(screen.takeTop(h, gapAfter), text, style);
  };
  auto titleStyle = theme.bodyText;
  titleStyle.align = fui::TextAlign::Center;
  titleStyle.bold = true;
  titleStyle.maxLines = 2;
  auto infoStyle = theme.smallText;
  infoStyle.align = fui::TextAlign::Center;
  infoStyle.maxLines = 2;

  if (openFailed || !shown.open) {
    line(statusLine, titleStyle, theme.spaceMd);
    return;
  }
  line(chapterTitle.c_str(), titleStyle, theme.spaceSm);
  line(chapterLine, infoStyle, theme.spaceSm);
  line(timeLine, infoStyle, theme.spaceSm);
  line(statusLine, infoStyle, theme.spaceMd);

  const int16_t rowHeight = theme.rowHeight;
  const int16_t gap = theme.spaceSm;
  const auto row = [&](std::initializer_list<std::pair<Control, const char*>> items) {
    const fui::Rect band = screen.takeTop(rowHeight, gap);
    const int count = static_cast<int>(items.size());
    const int16_t buttonWidth = static_cast<int16_t>((band.width - gap * (count - 1)) / count);
    int16_t x = band.x;
    for (const auto& [control, label] : items) {
      fui::ButtonProps button;
      button.label = label;
      button.action = ACTION_CONTROL;
      button.value = static_cast<int16_t>(control);
      button.styles = theme.button;
      button.styles.normal.border = fui::Paint::solid(fui::Color::Black);
      button.styles.normal.borderWidth = 1;
      button.text = theme.bodyText;
      button.radius = theme.controlRadius;
      button.minTouchSize = theme.minTouchSize;
      screen.button(button, fui::Rect{x, band.y, buttonWidth, rowHeight});
      x = static_cast<int16_t>(x + buttonWidth + gap);
    }
  };
  const bool headphones = halAudio.output() == HalAudio::Output::Headphones;
  row({{Control::Back30, tr(STR_AUDIOBOOK_BACK_30)},
       {Control::PlayPause, shown.playing || shown.waitingForOutput ? tr(STR_AUDIOBOOK_PAUSE) : tr(STR_AUDIOBOOK_PLAY)},
       {Control::Forward30, tr(STR_AUDIOBOOK_FORWARD_30)}});
  row({{Control::PreviousChapter, tr(STR_AUDIOBOOK_PREV_CHAPTER)},
       {Control::NextChapter, tr(STR_AUDIOBOOK_NEXT_CHAPTER)}});
  row({{Control::Speed, speedLabel}, {Control::Chapters, tr(STR_AUDIOBOOK_CHAPTERS)}});
  row({{Control::VolumeDown, tr(STR_AUDIOBOOK_VOLUME_DOWN)},
       {Control::Output, headphones ? tr(STR_AUDIOBOOK_HEADPHONES) : tr(STR_AUDIOBOOK_SPEAKER)},
       {Control::VolumeUp, tr(STR_AUDIOBOOK_VOLUME_UP)}});
  line(volumeLabel, infoStyle, 0);
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
  renderer.displayBuffer();
}

#endif
