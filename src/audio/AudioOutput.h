#pragma once

// App-level use of HalAudio: which output the user chose, saved headphones,
// and the main-loop poll of the Bluetooth audio module (Metalio only).
//
// The saved choice changes only by chooseOutput(), forgetHeadphones() or a
// pick that connects. A scan or a pick switches the module to headphones for
// a while; applySavedOutput() returns to the saved choice afterwards.

#if CROSSPOINT_AUDIOBOOK

#include <HalAudio.h>

namespace audiobook {

struct PlayerSnapshot;

// Opens the audio module link once (it stays open) and, the first time,
// applies the saved output. Does not block.
bool ensureOutput();

// Saves `output` as the user's choice and switches to it, after a running
// scan ends. Headphones connect to the saved address unless already linked or
// connecting.
void chooseOutput(HalAudio::Output output);

// Forgets the saved headphones and switches to the speaker.
void forgetHeadphones();

// Switches the module to headphones without saving, and scans. A playing
// book pauses itself (AudiobookService::tick()).
void scanForHeadphones();

// Connects to `address` from the scan list. It is saved, with the output set
// to headphones, once it connects, even if the screen has closed by then.
void pairWith(const char* address);

// After a scan or a pick: back to the saved choice once the module is free.
// Nothing changes if neither happened, or while a pick still connects.
void applySavedOutput();

// Polls the audio module and finishes picks and restores. Main loop, every
// pass; a pick is saved only while `storageFree` (not during USB Drive).
void tickOutput(bool storageFree);

// The module's output once anything used audio since boot, else the saved one.
HalAudio::Output currentOutput();

// Texts shared by the player, the control panel and the headphones screen.
// "Speaker", the saved headphones' name, or "Headphones".
const char* outputName();
// What the output is doing ("Switching output...", "Connecting...", the
// linked headset's name); empty when there is nothing to add.
const char* outputStatus();
// Playing, Paused, Finished, a problem, or waiting for the output.
const char* playbackStateText(const PlayerSnapshot& snapshot);

}  // namespace audiobook

#endif
