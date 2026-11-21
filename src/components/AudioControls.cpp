#include "AudioControls.h"

#if CROSSPOINT_AUDIOBOOK

#include <BookModel.h>
#include <FreeInkUIIcon.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "I18nKeys.h"
#include "components/icons/audioIcons.h"
#include "components/icons/customAudioIcons.h"

namespace fui = freeink::ui;

namespace {
struct SizedIcon {
  int16_t size;
  const freeink::Icon* icon;
};
// Largest first. Only the sizes the screens draw are listed (the play disc
// and skip buttons are 64 to 104 px), so only those bitmaps are linked.
const SizedIcon PLAY[] = {{48, &icon_play_filled_48}, {40, &icon_play_filled_40}};
const SizedIcon PAUSE[] = {{48, &icon_pause_filled_48}, {40, &icon_pause_filled_40}};
const SizedIcon PREVIOUS[] = {{32, &icon_skip_back_filled_32}, {24, &icon_skip_back_filled_24}};
const SizedIcon NEXT[] = {{32, &icon_skip_forward_filled_32}, {24, &icon_skip_forward_filled_24}};
const SizedIcon BACK[] = {{48, &icon_replay_48}, {40, &icon_replay_40}};
const SizedIcon FORWARD[] = {{48, &icon_forward_48}, {40, &icon_forward_40}};
const SizedIcon MINUS[] = {{24, &icon_minus_24}};
const SizedIcon PLUS[] = {{24, &icon_plus_24}};
const SizedIcon CHAPTERS[] = {{32, &icon_chapters_32}, {24, &icon_chapters_24}};
const SizedIcon SPEAKER[] = {{32, &icon_speaker_32}, {24, &icon_speaker_24}};
const SizedIcon HEADPHONES[] = {{32, &icon_headphones_32}, {24, &icon_headphones_24}};
const SizedIcon VOLUME_LOW[] = {{24, &icon_volume_low_24}};
const SizedIcon VOLUME_HIGH[] = {{24, &icon_volume_high_24}};
const SizedIcon BOOK[] = {{32, &icon_audiobook_32}};

template <size_t N>
fui::BitmapRef pick(const SizedIcon (&sizes)[N], const int16_t size) {
  for (const auto& sized : sizes) {
    if (sized.size <= size) return fui::bitmapFromIcon(*sized.icon);
  }
  return fui::bitmapFromIcon(*sizes[N - 1].icon);
}

// The icon size for a button `side` pixels across.
int16_t iconSizeFor(const int16_t side) { return side >= 96 ? 48 : side >= 72 ? 40 : side >= 56 ? 32 : 24; }
// The skip icons hold the "30", so they get one size more.
int16_t skipIconSizeFor(const int16_t side) { return side >= 72 ? 48 : side >= 64 ? 40 : 32; }

constexpr uint8_t BORDER_WIDTH = 2;
constexpr uint8_t PLAY_BORDER_WIDTH = 3;

// "4:05", or "1:04:05" from an hour on.
void formatClock(char* out, const size_t size, const uint64_t ms) {
  const auto seconds = static_cast<unsigned long>(ms / 1000);
  const unsigned long hours = seconds / 3600;
  if (hours > 0) {
    snprintf(out, size, "%lu:%02lu:%02lu", hours, (seconds / 60) % 60, seconds % 60);
  } else {
    snprintf(out, size, "%lu:%02lu", seconds / 60, seconds % 60);
  }
}

}  // namespace

fui::BitmapRef AudioControls::icon(const Icon icon, const int16_t size) {
  switch (icon) {
    case Icon::Play:
      return pick(PLAY, size);
    case Icon::Pause:
      return pick(PAUSE, size);
    case Icon::PreviousChapter:
      return pick(PREVIOUS, size);
    case Icon::NextChapter:
      return pick(NEXT, size);
    case Icon::Back:
      return pick(BACK, size);
    case Icon::Forward:
      return pick(FORWARD, size);
    case Icon::Minus:
      return pick(MINUS, size);
    case Icon::Plus:
      return pick(PLUS, size);
    case Icon::Chapters:
      return pick(CHAPTERS, size);
    case Icon::Speaker:
      return pick(SPEAKER, size);
    case Icon::Headphones:
      return pick(HEADPHONES, size);
    case Icon::VolumeLow:
      return pick(VOLUME_LOW, size);
    case Icon::VolumeHigh:
      return pick(VOLUME_HIGH, size);
    case Icon::Audiobook:
      return pick(BOOK, size);
  }
  return {};
}

void AudioControls::prepareButton(const Screen& screen) {
  const auto& theme = screen.theme();
  button_.label = nullptr;
  button_.iconSize = 0;
  button_.inputMask = fui::InputTouch;
  button_.minTouchSize = theme.minTouchSize;
  button_.radius = theme.controlRadius;
  button_.styles = theme.button;
  button_.styles.normal.border = fui::Paint::solid(fui::Color::Black);
  button_.styles.normal.borderWidth = BORDER_WIDTH;
  // Disabled: the outline goes and the icon stays readable. Icons cannot be
  // drawn gray, so the missing outline is what marks the end of a range.
  button_.styles.disabled.background = fui::Paint::solid(fui::Color::White);
  button_.styles.disabled.foreground = fui::Paint::solid(fui::Color::Black);
  button_.styles.disabled.border = fui::Paint::none();
  button_.styles.disabled.borderWidth = 0;
}

void AudioControls::iconButton(Screen& screen, const fui::Rect rect, const Icon which, const fui::ActionId action,
                               const int16_t value, const bool enabled) {
  prepareButton(screen);
  button_.icon = icon(which, iconSizeFor(std::min(rect.width, rect.height)));
  button_.action = action;
  button_.value = value;
  button_.enabled = enabled;
  screen.button(button_, rect);
}

void AudioControls::skipButton(Screen& screen, const fui::Rect rect, const Icon which, const fui::ActionId action,
                               const int16_t value) {
  prepareButton(screen);
  button_.icon = icon(which, skipIconSizeFor(std::min(rect.width, rect.height)));
  button_.action = action;
  button_.value = value;
  button_.enabled = true;
  screen.button(button_, rect);

  // "30" inside the circular arrow, in the button's current ink: white while
  // the tap flash fills the button black.
  auto style = screen.theme().smallText;
  style.align = fui::TextAlign::Center;
  style.bold = true;
  style.maxLines = 1;
  const fui::State state = screen.frame().stateFor(action, value, fui::StateNormal);
  if (fui::hasState(state, fui::StateActive) || fui::hasState(state, fui::StateSelected)) {
    style.color = fui::Color::White;
  }
  static constexpr const char* LABEL = "30";
  const fui::Rect ink = screen.target().inkBounds(style.font, LABEL, style);
  const int16_t top = static_cast<int16_t>(rect.y + rect.height / 2 - ink.y - ink.height / 2);
  screen.target().text(fui::Rect{rect.x, top, rect.width, screen.target().lineHeight(style.font)}, LABEL, style);
}

void AudioControls::playButton(Screen& screen, const fui::Rect rect, const fui::ActionId action, const int16_t value,
                               const PlayState state) {
  prepareButton(screen);
  const int16_t side = std::min(rect.width, rect.height);
  button_.radius = static_cast<uint8_t>(side / 2);  // a disc
  auto& normal = button_.styles.normal;
  normal.borderWidth = PLAY_BORDER_WIDTH;
  switch (state) {
    case PlayState::Playing:
      normal.background = fui::Paint::solid(fui::Color::Black);
      normal.foreground = fui::Paint::solid(fui::Color::White);
      // The tap flash inverts it, as it does the outlined buttons.
      button_.styles.active.background = fui::Paint::solid(fui::Color::White);
      button_.styles.active.foreground = fui::Paint::solid(fui::Color::Black);
      break;
    case PlayState::Waiting:
      normal.background = fui::Paint::dither(fui::Color::LightGray);
      break;
    case PlayState::Paused:
      break;
  }
  button_.icon = icon(state == PlayState::Paused ? Icon::Play : Icon::Pause, iconSizeFor(side));
  button_.action = action;
  button_.value = value;
  button_.enabled = true;
  screen.button(button_, rect);
}

void AudioControls::transportRow(Screen& screen, const fui::Rect band, const TransportSizes& sizes,
                                 const fui::ActionId action, const TransportValues& values, const PlayState state) {
  const int16_t total = static_cast<int16_t>(2 * sizes.chapter + 2 * sizes.skip + sizes.play + 4 * sizes.gap);
  int16_t x = static_cast<int16_t>(band.x + (band.width - total) / 2);
  const int16_t centerY = static_cast<int16_t>(band.y + band.height / 2);
  const auto place = [&x, centerY, &sizes](const int16_t size) {
    const fui::Rect rect{x, static_cast<int16_t>(centerY - size / 2), size, size};
    x = static_cast<int16_t>(x + size + sizes.gap);
    return rect;
  };
  iconButton(screen, place(sizes.chapter), Icon::PreviousChapter, action, values.previousChapter);
  skipButton(screen, place(sizes.skip), Icon::Back, action, values.back);
  playButton(screen, place(sizes.play), action, values.playPause, state);
  skipButton(screen, place(sizes.skip), Icon::Forward, action, values.forward);
  iconButton(screen, place(sizes.chapter), Icon::NextChapter, action, values.nextChapter);
}

void AudioControls::slimSlider(Screen& screen, const fui::Rect rect, const int32_t position, const int32_t max,
                               const fui::ActionId action, const int16_t value, const bool enabled) {
  fui::SliderProps props;
  props.value = position;
  props.max = max;
  props.action = action;
  props.actionValue = value;
  props.inputMask = fui::InputTouch | fui::InputDrag;
  props.trackHeight = 6;
  props.knobWidth = 16;
  props.knobHeight = 28;
  // No padding: the drag maps over the whole band, so the knob stays within
  // half its width of the finger.
  props.horizontalPadding = 0;
  props.minTouchSize = screen.theme().minTouchSize;
  props.radius = 3;
  props.enabled = enabled;
  fui::slider(screen.frame(), rect, props);
}

AudioControls::PlayState AudioControls::playState(const audiobook::PlayerSnapshot& shown) {
  if (shown.waitingForOutput) return PlayState::Waiting;
  return shown.playing ? PlayState::Playing : PlayState::Paused;
}

void AudioControls::ChapterBar::show(const audiobook::PlayerSnapshot& shown) {
  chapter_ = shown.open ? shown.chapter : 0;
  elapsedMs_ = shown.open ? shown.chapterElapsedMs : 0;
  lengthMs_ = shown.open ? shown.chapterLengthMs : 0;
  format();
}

bool AudioControls::ChapterBar::stale(const audiobook::PlayerSnapshot& now) const {
  const uint64_t length = now.open ? now.chapterLengthMs : 0;
  const auto shown = audiobook::chapterMinutes(elapsedMs_, lengthMs_);
  const auto next = audiobook::chapterMinutes(now.open ? now.chapterElapsedMs : 0, length);
  return next.elapsed != shown.elapsed || next.length != shown.length || (length == 0) != (lengthMs_ == 0);
}

bool AudioControls::ChapterBar::moves(const int16_t permille) const {
  const int16_t clamped = permille < 0 ? 0 : permille > 1000 ? 1000 : permille;
  return !dragging_ || clamped != permille_;
}

bool AudioControls::ChapterBar::drag(const int16_t permille) {
  const bool starting = !dragging_;
  if (starting) {
    // The slider takes no taps while the length is unknown.
    if (lengthMs_ == 0) return false;
    dragging_ = true;
    dragChapter_ = chapter_;
    dragLengthMs_ = lengthMs_;
  }
  const uint64_t shownSeconds = targetMs() / 1000;
  permille_ = permille < 0 ? 0 : permille > 1000 ? 1000 : permille;
  const bool changed = starting || targetMs() / 1000 != shownSeconds;
  if (changed) format();
  return changed;
}

bool AudioControls::ChapterBar::pendingSeek(size_t& chapter, uint64_t& targetMs) const {
  if (!dragging_) return false;
  chapter = dragChapter_;
  targetMs = this->targetMs();
  return true;
}

void AudioControls::ChapterBar::endDrag() {
  if (!dragging_) return;
  dragging_ = false;
  format();
}

uint64_t AudioControls::ChapterBar::targetMs() const {
  return audiobook::chapterSeekOffsetMs(dragLengthMs_, permille_);
}

void AudioControls::ChapterBar::format() {
  if (dragging_) {
    // The drag target, to the second.
    const uint64_t target = targetMs();
    formatClock(elapsed_, sizeof(elapsed_), target);
    char clock[16];
    formatClock(clock, sizeof(clock), dragLengthMs_ > target ? dragLengthMs_ - target : 0);
    snprintf(left_, sizeof(left_), "-%s", clock);
    return;
  }
  const auto minutes = audiobook::chapterMinutes(elapsedMs_, lengthMs_);
  snprintf(elapsed_, sizeof(elapsed_), tr(STR_AUDIOBOOK_MINUTES), static_cast<unsigned>(minutes.elapsed));
  if (lengthMs_ == 0) {
    left_[0] = '\0';
    return;
  }
  snprintf(left_, sizeof(left_), tr(STR_AUDIOBOOK_MINUTES_LEFT), static_cast<unsigned>(minutes.left));
}

void AudioControls::ChapterBar::slider(Screen& screen, const fui::Rect rect, const fui::ActionId action,
                                       const int16_t value) const {
  const bool known = lengthMs_ > 0;
  int32_t position = 0;
  int32_t max = 1;
  if (dragging_) {
    position = permille_;
    max = 1000;
  } else if (known) {
    position = static_cast<int32_t>(elapsedMs_ / 1000);
    max = static_cast<int32_t>(lengthMs_ / 1000);
  }
  slimSlider(screen, rect, position, max, action, value, known);
}

#endif
