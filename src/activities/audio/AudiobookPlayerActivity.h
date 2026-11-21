#pragma once

#if CROSSPOINT_AUDIOBOOK

#include <PcmPipeline.h>

#include <string>

#include "activities/Activity.h"
#include "audio/AudiobookService.h"
#include "components/UiAppHost.h"

// Now-playing screen for AudiobookService. Leaving it keeps the book playing;
// the book is reached again from Recents or the file browser.
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
    Speed,
    Chapters,
    Output,
    VolumeDown,
    VolumeUp,
  };

  static void screenFn(UiScreen& screen, void* user);
  static void onControl(const freeink::ui::ActionEvent& event, void* user);
  void buildScreen(UiScreen& screen);
  void handle(Control control);
  void changeVolume(int delta);
  void refresh();

  std::string path;
  bool autoPlay = true;
  bool openFailed = false;
  uint32_t shownRevision = 0;
  uint64_t shownMinute = UINT64_MAX;

  // Copied on the main loop under the render lock; read by buildScreen.
  audiobook::PlayerSnapshot shown;
  std::string title;
  std::string chapterTitle;
  char chapterLine[48] = {};
  char timeLine[48] = {};
  char statusLine[96] = {};
  char speedLabel[24] = {};
  char volumeLabel[24] = {};
};

#endif
