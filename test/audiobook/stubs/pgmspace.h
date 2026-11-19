#pragma once
// Host stand-in: program-memory tables are ordinary memory off-device.
#include <stdint.h>
#define PROGMEM
#define pgm_read_byte(addr) (*(const uint8_t*)(addr))
#define pgm_read_word(addr) (*(const uint16_t*)(addr))
