#pragma once

#if CROSSPOINT_AUDIOBOOK

#include <GfxRenderer.h>

#include <string>
#include <vector>

#include "activities/UiListActivity.h"

class MappedInputManager;

// Audiobook output: speaker or Bluetooth headphones, scanning, picking and
// forgetting headphones, a test sound and a log of the module's replies. The audio module belongs to
// HalAudio; this screen asks it and saves the user's choice.
class BtHeadphonesActivity final : public UiListActivity {
 public:
  explicit BtHeadphonesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("BtHeadphones", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  int listCount() const override { return static_cast<int>(rows.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  void onBackButton() override;

  enum class View { Menu, Scan, Log };
  void showView(View target);
  void refresh();
  void activateMenu(int code);
  void saveConnectedHeadset();

  View view = View::Menu;
  bool audioOpen = false;
  uint32_t shownRevision = 0;
  unsigned long lastRepaintMs = 0;
  // Test Sound waits for HalAudio::wakeOutput() to find the module's clock.
  bool toneWaiting = false;
  unsigned long toneSinceMs = 0;
  char pickedAddress[13] = {};

  // Written on the main loop under the render lock, read by buildScreen.
  std::vector<freeink::ui::ListItem> rows;
  std::vector<std::string> labels;
  std::vector<std::string> values;  // Log view times, parallel to labels
  std::string status;
};

#endif
