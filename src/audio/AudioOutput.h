#pragma once

// App-level use of HalAudio: which output the user chose, saved headphones,
// and the main-loop poll of the Bluetooth audio module (Metalio only).

#if CROSSPOINT_AUDIOBOOK

namespace audiobook {

// Opens the audio path and, on first open, applies the saved output: the
// speaker, or the saved headphones. Each success needs one releaseOutput().
bool acquireOutput();
void releaseOutput();

// Polls the audio module. Called from the main loop on every pass.
void tickOutput();

}  // namespace audiobook

#endif
