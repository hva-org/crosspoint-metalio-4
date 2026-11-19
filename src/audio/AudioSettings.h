#pragma once

// Saved audiobook player choices (Metalio only), stored as the "audio" object
// in settings.json. Missing or invalid values keep the defaults.

#include <ArduinoJson.h>
#include <BtModuleProtocol.h>
#include <PcmPipeline.h>

#include <cstdint>
#include <cstring>

namespace audiobook {

struct AudioSettings {
  static constexpr uint8_t OUTPUT_SPEAKER = 0;
  static constexpr uint8_t OUTPUT_HEADPHONES = 1;
  static constexpr uint8_t DEFAULT_VOLUME = 60;
  static constexpr uint8_t VOLUME_STEP = 10;

  uint8_t output = OUTPUT_SPEAKER;
  uint8_t speedIndex = DEFAULT_SPEED_INDEX;
  uint8_t volume = DEFAULT_VOLUME;  // 0-100
  char headsetAddress[metalio_bt::ADDRESS_LENGTH + 1] = {};
  char headsetName[metalio_bt::NAME_CAPACITY] = {};
};

inline void writeJson(const AudioSettings& s, JsonDocument& doc) {
  JsonObject audio = doc["audio"].to<JsonObject>();
  audio["output"] = s.output;
  audio["speed"] = s.speedIndex;
  audio["volume"] = s.volume;
  audio["headsetAddr"] = s.headsetAddress;
  audio["headsetName"] = s.headsetName;
}

inline void readJson(AudioSettings& s, JsonVariantConst doc) {
  const JsonVariantConst audio = doc["audio"];
  if (audio.isNull()) return;
  const uint8_t output = audio["output"] | s.output;
  if (output <= AudioSettings::OUTPUT_HEADPHONES) s.output = output;
  const uint8_t speed = audio["speed"] | s.speedIndex;
  if (speed < PLAYBACK_SPEED_COUNT) s.speedIndex = speed;
  const uint8_t volume = audio["volume"] | s.volume;
  if (volume <= 100) s.volume = volume;
  // A missing or malformed address clears the saved headphones entirely.
  const char* address = audio["headsetAddr"] | "";
  if (metalio_bt::isValidAddress(address)) {
    memcpy(s.headsetAddress, address, sizeof(s.headsetAddress));
    strncpy(s.headsetName, audio["headsetName"] | "", sizeof(s.headsetName) - 1);
    s.headsetName[sizeof(s.headsetName) - 1] = '\0';
  } else {
    s.headsetAddress[0] = '\0';
    s.headsetName[0] = '\0';
  }
}

}  // namespace audiobook
