/* EchoPoint build configuration for faad2. Not an upstream file: libfaad's
   common.h includes it when HAVE_CONFIG_H is defined (see library.json). */
#ifndef FAAD2_ECHOPOINT_CONFIG_H
#define FAAD2_ECHOPOINT_CONFIG_H

#define PACKAGE_VERSION "2.11.4"
#define HAVE_INTTYPES_H 1
#define HAVE_STRING_H 1

/* AAC-LC with SBR (HE-AAC v1) only: no Main, LTP, LD, SSR, ER or DRM. */
#define LC_ONLY_DECODER

/* No parametric stereo (HE-AAC v2). Needs the common.h patch in README.md. */
#define DISABLE_PS

/* SBR gain square roots run per band and envelope: keep them in single
   precision on the FPU instead of software double. */
#define HAVE_SQRTF 1

/* Decoder state goes to PSRAM only, like the player's other buffers. Plain
   malloc() would put each block of 4 KB or less, about 30 KB for stereo
   HE-AAC, in internal RAM. Needs the common.c patch in README.md. */
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#define FAAD_MALLOC(size) heap_caps_malloc((size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#endif

#endif
