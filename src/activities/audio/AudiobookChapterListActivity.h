#pragma once

#if CROSSPOINT_AUDIOBOOK

#include <vector>

#include "activities/UiListActivity.h"
#include "audio/AudiobookService.h"

// Lists the open audiobook's chapters (or MP3 files); picking one jumps there.
class AudiobookChapterListActivity final : public UiListActivity {
 public:
  AudiobookChapterListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("AudiobookChapters", renderer, mappedInput) {}

  void onEnter() override;

 private:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;

  // Copied at entry on the main loop; the render task reads only this copy.
  std::vector<audiobook::ChapterTitle> titles;
};

#endif
