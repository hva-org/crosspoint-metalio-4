#include "AudioOutput.h"

#if CROSSPOINT_AUDIOBOOK

#include <HalAudio.h>

#include "CrossPointSettings.h"

namespace audiobook {

bool acquireOutput() {
  const bool first = !halAudio.isOpen();
  const auto wanted = SETTINGS.audio.output == AudioSettings::OUTPUT_HEADPHONES ? HalAudio::Output::Headphones
                                                                                : HalAudio::Output::Speaker;
  if (!halAudio.acquire(wanted)) return false;
  // Sends only what the module's current mode still needs.
  if (first) halAudio.selectOutput(wanted, SETTINGS.audio.headsetAddress);
  return true;
}

void releaseOutput() { halAudio.release(); }

void tickOutput() { halAudio.poll(); }

}  // namespace audiobook

#endif
