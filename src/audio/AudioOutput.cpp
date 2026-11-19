#include "AudioOutput.h"

#if CROSSPOINT_AUDIOBOOK

#include <I18n.h>

#include <cstring>

#include "AudiobookService.h"
#include "CrossPointSettings.h"
#include "I18nKeys.h"

namespace audiobook {
namespace {
// What the module follows instead of the saved choice.
enum class Hold : uint8_t {
  None,             // the saved choice
  Temporary,        // a scan or pick, while the headphones screen is open
  RestoreWhenFree,  // back to the saved choice once no scan or connect runs
};
Hold hold = Hold::None;
// The scanned headset being connected, saved once linked; empty when none.
char pendingPick[metalio_bt::ADDRESS_LENGTH + 1] = {};

HalAudio::Output savedOutput() {
  return SETTINGS.audio.output == AudioSettings::OUTPUT_HEADPHONES ? HalAudio::Output::Headphones
                                                                   : HalAudio::Output::Speaker;
}

// Puts the module on the saved choice. During a scan it waits for the scan
// to end: mode commands in the middle of an inquiry were never tried.
void applySaved() {
  pendingPick[0] = '\0';
  if (halAudio.link() == HalAudio::Link::Scanning) {
    hold = Hold::RestoreWhenFree;
    return;
  }
  hold = Hold::None;
  halAudio.selectOutput(savedOutput(), SETTINGS.audio.headsetAddress);
}

void savePick() {
  auto& audio = SETTINGS.audio;
  strncpy(audio.headsetAddress, pendingPick, sizeof(audio.headsetAddress) - 1);
  audio.headsetAddress[sizeof(audio.headsetAddress) - 1] = '\0';
  strncpy(audio.headsetName, halAudio.connectedName(), sizeof(audio.headsetName) - 1);
  audio.headsetName[sizeof(audio.headsetName) - 1] = '\0';
  audio.output = AudioSettings::OUTPUT_HEADPHONES;
  SETTINGS.saveToFile();
}
}  // namespace

bool ensureOutput() {
  if (halAudio.isOpen()) return true;
  if (!halAudio.begin()) return false;
  halAudio.selectOutput(savedOutput(), SETTINGS.audio.headsetAddress);
  return true;
}

void chooseOutput(const HalAudio::Output output) {
  const uint8_t value =
      output == HalAudio::Output::Headphones ? AudioSettings::OUTPUT_HEADPHONES : AudioSettings::OUTPUT_SPEAKER;
  if (SETTINGS.audio.output != value) {
    SETTINGS.audio.output = value;
    SETTINGS.saveToFile();
  }
  if (ensureOutput()) applySaved();
}

void forgetHeadphones() {
  auto& audio = SETTINGS.audio;
  audio.headsetAddress[0] = '\0';
  audio.headsetName[0] = '\0';
  // Back to the speaker, as the reference firmware does. That also drops the
  // headset link, so the headphones can be found by a scan again.
  audio.output = AudioSettings::OUTPUT_SPEAKER;
  SETTINGS.saveToFile();
  if (ensureOutput()) applySaved();
}

void scanForHeadphones() {
  if (!ensureOutput() || halAudio.isBusy()) return;
  pendingPick[0] = '\0';
  hold = Hold::Temporary;
  // No address: a connect would keep the scan from starting.
  halAudio.selectOutput(HalAudio::Output::Headphones, nullptr);
  halAudio.startScan();
}

void pairWith(const char* address) {
  if (!ensureOutput() || halAudio.isBusy() || !metalio_bt::isValidAddress(address)) return;
  hold = Hold::Temporary;
  halAudio.selectOutput(HalAudio::Output::Headphones, nullptr);
  halAudio.connect(address);
  strncpy(pendingPick, address, sizeof(pendingPick) - 1);
  pendingPick[sizeof(pendingPick) - 1] = '\0';
}

void applySavedOutput() {
  if (hold == Hold::Temporary) hold = Hold::RestoreWhenFree;
}

void tickOutput(const bool storageFree) {
  halAudio.poll();
  if (pendingPick[0] != '\0') {
    if (halAudio.headsetConnected() && strcmp(halAudio.connectedAddress(), pendingPick) == 0) {
      // USB Drive owns the card meanwhile; the pick waits for it.
      if (!storageFree) return;
      savePick();
      pendingPick[0] = '\0';
      hold = Hold::None;
      return;
    }
    // Failed, timed out, or replaced by another choice: nothing is saved.
    if (halAudio.output() != HalAudio::Output::Headphones ||
        (!halAudio.isBusy() && halAudio.link() != HalAudio::Link::Connecting)) {
      pendingPick[0] = '\0';
    }
  }
  // Waits for a running scan or connect to finish, as the module expects.
  if (hold == Hold::RestoreWhenFree && pendingPick[0] == '\0' && !halAudio.isBusy()) applySaved();
}

HalAudio::Output currentOutput() { return halAudio.isOpen() ? halAudio.output() : savedOutput(); }

const char* outputName() {
  if (currentOutput() != HalAudio::Output::Headphones) return tr(STR_AUDIOBOOK_SPEAKER);
  return SETTINGS.audio.headsetName[0] != '\0' ? SETTINGS.audio.headsetName : tr(STR_AUDIOBOOK_HEADPHONES);
}

const char* outputStatus() {
  if (!halAudio.isOpen()) return "";  // nothing has used audio since boot
  const auto& saved = SETTINGS.audio;
  if (halAudio.output() != HalAudio::Output::Headphones) {
    return halAudio.modeSettled() ? "" : tr(STR_AUDIOBOOK_SWITCHING);
  }
  switch (halAudio.link()) {
    case HalAudio::Link::Connected:
      if (saved.headsetName[0] != '\0' && strcmp(halAudio.connectedAddress(), saved.headsetAddress) == 0) {
        return saved.headsetName;
      }
      return halAudio.connectedName()[0] != '\0' ? halAudio.connectedName() : halAudio.connectedAddress();
    case HalAudio::Link::Connecting:
      return tr(STR_CONNECTING);
    case HalAudio::Link::Scanning:
      return tr(STR_SCANNING);
    case HalAudio::Link::Failed:
      return tr(STR_CONNECTION_FAILED);
    case HalAudio::Link::None:
      break;
  }
  // Headset mode is entered by the connect or scan that needs it.
  return saved.headsetAddress[0] != '\0' ? tr(STR_AUDIOBOOK_NOT_CONNECTED) : tr(STR_AUDIOBOOK_PICK_HEADPHONES);
}

const char* playbackStateText(const PlayerSnapshot& snapshot) {
  if (snapshot.problem == PlaybackProblem::Track) return tr(STR_AUDIOBOOK_TRACK_FAILED);
  if (snapshot.problem == PlaybackProblem::Output) return tr(STR_AUDIOBOOK_NO_OUTPUT);
  if (snapshot.waitingForOutput) {
    return halAudio.output() == HalAudio::Output::Headphones ? tr(STR_AUDIOBOOK_CONNECTING_HEADPHONES)
                                                             : tr(STR_AUDIOBOOK_SWITCHING);
  }
  if (snapshot.finished) return tr(STR_AUDIOBOOK_FINISHED);
  return snapshot.playing ? tr(STR_AUDIOBOOK_PLAYING) : tr(STR_AUDIOBOOK_PAUSED);
}

}  // namespace audiobook

#endif
