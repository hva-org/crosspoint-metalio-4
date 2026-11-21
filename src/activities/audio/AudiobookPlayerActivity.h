#pragma once

#if CROSSPOINT_AUDIOBOOK

#include <PcmPipeline.h>

#include <string>

#include "activities/Activity.h"
#include "audio/AudiobookService.h"
#include "components/AudioControls.h"
#include "components/UiAppHost.h"

// Now-playing screen for AudiobookService. Leaving it keeps the book playing;
// the book is reached again from Recents, the file browser or the control
// panel.
class AudiobookPlayerActivity final : public Activity, private UiAppHost {
 public:
  // autoPlay starts the book when it was not already open.
  AudiobookPlayerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path, bool autoPlay)
      : Activity("AudiobookPlayer", renderer, mappedInput),
        UiAppHost(renderer),
        path(std::move(path)),
        autoPlay(autoPlay) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return AUDIOBOOK.isPlaying(); }

 private:
  enum class Control : int16_t {
    Back30,
    PlayPause,
    Forward30,
    PreviousChapter,
    NextChapter,
    SpeedDown,
    SpeedUp,
    Chapters,
    Output,
    VolumeDown,
    VolumeUp,
    VolumeSet,    // slider; the event's dragPermille is the level
    ChapterSeek,  // slider; applied when the finger lifts
  };

  static void screenFn(UiScreen& screen, void* user);
  static void onControl(const freeink::ui::ActionEvent& event, void* user);
  void buildScreen(UiScreen& screen);
  void buildTall(UiScreen& screen);
  void buildWide(UiScreen& screen);
  void drawChapterBar(UiScreen& screen, freeink::ui::Rect slider, freeink::ui::Rect labels);
  void extrasRow(UiScreen& screen, freeink::ui::Rect band);
  void volumeRow(UiScreen& screen, freeink::ui::Rect band);
  void handle(Control control, int16_t dragPermille);
  void changeVolume(int delta);
  // endDrag also ends a chapter drag and asks for a cleaning refresh, in the
  // same locked step that takes the position.
  void refresh(bool endDrag = false);

  std::string path;
  bool autoPlay = true;
  bool openFailed = false;
  uint32_t shownRevision = 0;
  // A slider was dragged or tapped: one cleaning refresh once the finger lifts.
  bool sliderTouched = false;
  bool cleanRefreshPending = false;

  // Holds the button props (a 324-byte StyleSet) off the render stack.
  AudioControls controls;
  // Written on the main loop under the render lock; read by buildScreen.
  AudioControls::ChapterBar chapterBar;
  audiobook::PlayerSnapshot shown;
  std::string title;
  std::string chapterTitle;
  char chapterLine[48] = {};
  char statusLine[96] = {};
  char speedLabel[16] = {};
  char volumeLabel[8] = {};
};

#endif
