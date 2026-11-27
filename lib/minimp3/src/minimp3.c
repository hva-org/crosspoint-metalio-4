/* Not an upstream file: builds the header-only minimp3 decoder once, for
   Layer III only and with portable C (Xtensa has no SSE or NEON). */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3.h"
