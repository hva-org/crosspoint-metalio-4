#pragma once

#if CROSSPOINT_AUDIOBOOK

#include <cstddef>
#include <cstdint>

#include "audio/AudiobookService.h"
#include "components/UiAppHost.h"

// Controls shared by the audiobook player and the control panel, so both
// screens draw the same icons the same way. Immediate mode: call these from a
// screen build. The icons live in this file's .cpp only, so each is linked once.
class AudioControls {
 public:
  using Screen = UiAppHost::UiScreen;

  enum class Icon : uint8_t {
    Play,
    Pause,
    PreviousChapter,
    NextChapter,
    Back,
    Forward,
    Minus,
    Plus,
    Chapters,
    Speaker,
    Headphones,
    VolumeLow,
    VolumeHigh,
    Audiobook,
  };
  // The icon at the largest generated size that fits in `size` pixels.
  static freeink::ui::BitmapRef icon(Icon icon, int16_t size);

  enum class PlayState : uint8_t { Paused, Playing, Waiting };

  // Button sizes of the transport row, in pixels.
  struct TransportSizes {
    int16_t chapter;  // previous / next chapter
    int16_t skip;     // 30 s back / forward
    int16_t play;     // the play/pause disc
    int16_t gap;      // between buttons; gaps take no taps
  };
  // The value each transport button sends with the row's action.
  struct TransportValues {
    int16_t previousChapter;
    int16_t back;
    int16_t playPause;
    int16_t forward;
    int16_t nextChapter;
  };

  // An outlined icon button. Disabled: no outline and no tap.
  void iconButton(Screen& screen, freeink::ui::Rect rect, Icon icon, freeink::ui::ActionId action, int16_t value,
                  bool enabled = true);
  // [|<] [<30] (play) [30>] [>|], centered in `band`. The play disc is black
  // while playing, gray while waiting for the output, outlined while paused.
  void transportRow(Screen& screen, freeink::ui::Rect band, const TransportSizes& sizes, freeink::ui::ActionId action,
                    const TransportValues& values, PlayState state);
  // A thin bar with a knob across `rect`, which is also the touch band. Taps
  // and drags send `action` and `value` with ActionEvent::dragPermille.
  // Disabled: gray, and no tap.
  static void slimSlider(Screen& screen, freeink::ui::Rect rect, int32_t position, int32_t max,
                         freeink::ui::ActionId action, int16_t value, bool enabled = true);

  static PlayState playState(const audiobook::PlayerSnapshot& shown);

  // The chapter slider and its two time labels, as both screens show them.
  // While the finger is down the labels show the drag target to the second;
  // the seek is applied once, on release, so the decoder does not restart on
  // every frame. Otherwise they show whole minutes (see chapterMinutes()).
  // Main loop only; screens call the state changes under their render lock.
  class ChapterBar {
   public:
    // Takes the position to show.
    void show(const audiobook::PlayerSnapshot& shown);
    // True when `now` would show other minutes or another length than the
    // last show(): the screen needs a refresh.
    bool stale(const audiobook::PlayerSnapshot& now) const;
    // False when drag(permille) would change nothing; safe without the lock.
    bool moves(int16_t permille) const;
    // Starts a drag in the shown chapter, or moves it. Returns whether the
    // shown target changed.
    bool drag(int16_t permille);
    // The seek a running drag would apply; false when none runs. Read-only,
    // so it needs no lock: apply the seek outside the render lock (it writes
    // the saved position), then call endDrag() with the new position.
    bool pendingSeek(size_t& chapter, uint64_t& targetMs) const;
    void endDrag();
    bool dragging() const { return dragging_; }
    // The slider across `rect`; gray and without taps while the length is
    // unknown.
    void slider(Screen& screen, freeink::ui::Rect rect, freeink::ui::ActionId action, int16_t value) const;
    const char* elapsedLabel() const { return elapsed_; }
    const char* leftLabel() const { return left_; }

   private:
    uint64_t targetMs() const;
    void format();

    size_t chapter_ = 0;
    uint64_t elapsedMs_ = 0;
    uint64_t lengthMs_ = 0;  // 0 while unknown
    bool dragging_ = false;
    size_t dragChapter_ = 0;
    uint64_t dragLengthMs_ = 0;
    int16_t permille_ = 0;
    char elapsed_[24] = {};
    char left_[24] = {};
  };

 private:
  void prepareButton(const Screen& screen);
  void skipButton(Screen& screen, freeink::ui::Rect rect, Icon icon, freeink::ui::ActionId action, int16_t value);
  void playButton(Screen& screen, freeink::ui::Rect rect, freeink::ui::ActionId action, int16_t value, PlayState state);

  // Embeds a 324-byte StyleSet; a member keeps it off the render stack.
  freeink::ui::ButtonProps button_;
};

#endif
