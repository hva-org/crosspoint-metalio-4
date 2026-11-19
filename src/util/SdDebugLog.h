#pragma once

// Debug log on the SD card: with Settings > System > Debug Log to SD Card on,
// every log line (LOG_LEVEL of the build, so debug lines on dev builds) is
// appended to /.crosspoint/debug.log. Above 2 MB the file moves to
// debug.old.log and a new one starts.
//
// logPrintf() only copies each line into a PSRAM buffer, from whatever task
// logs it; the main loop writes the buffer to the card about once a second.
// Lines logged while the buffer is full are counted and reported as dropped.

#if CROSSPOINT_SD_LOG

namespace sdlog {

// Main loop: starts or stops with the setting, and writes buffered lines
// unless another part of the firmware owns the card (`storageFree` false).
void tick(bool storageFree);

// Writes everything buffered now, for deep sleep or a restart.
void flush();

}  // namespace sdlog

#endif
