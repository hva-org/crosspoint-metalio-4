# FAAD2 AAC decoder

[FAAD2](https://github.com/knik0/faad2) release `2.11.4` (commit
`e8e76f0a44db45aed773a3f62fe35a63c867a738`). `README`, `COPYING` and
`AUTHORS` are upstream's.

License: GPL-2.0-or-later, see `COPYING`. Upstream asks that the copyright
notice read: "Code from FAAD2 is copyright (c) Nero AG, www.nero.com".

## Files kept

- `include/neaacdec.h`
- `libfaad/`: every `.c` and `.h` file and `codebook/`, except the files that
  this configuration compiles to nothing: `drm_dec.*`, `hcr.c`,
  `ic_predict.c`, `lt_predict.c`, `rvlc.*`, `ssr*`, `ps_dec.*`,
  `ps_syntax.c` and `ps_tables.h`. `ic_predict.h` and `lt_predict.h` stay
  because `specrec.c` includes them. `fixed.h` stays for `FIXED_POINT`
  builds.

The frontend, build files, fuzzers and docs are left out.

## Build configuration

`library.json` defines `HAVE_CONFIG_H`, so `libfaad/common.h` includes the
local `config.h`. It selects:

- `LC_ONLY_DECODER`: AAC-LC, with SBR (HE-AAC v1). No Main, LTP, LD, SSR,
  error-resilient or DRM profiles; no 960-sample frames.
- `DISABLE_PS`: no parametric stereo (HE-AAC v2). The player mixes every
  stream to mono, and a PS stream's core is already a mono mix, so PS would
  cost flash, CPU and about 46 KB more decoder stack for no audible gain.
  `AudioDecoder` passes object type 29 configs to faad2 as type 5.
- Floating point (`FIXED_POINT` is not defined). The ESP32-S3 has a
  single-precision FPU; fixed-point builds turn each multiply into a 64-bit
  integer sequence.
- `HAVE_SQRTF`: SBR gain square roots in single precision.
- `APPLY_DRC` is not defined: dynamic range control data is read but not
  applied.
- `FAAD_MALLOC` (ESP-IDF builds only): decoder state is allocated in PSRAM
  only. Plain `malloc()` puts blocks of 4 KB or less in internal RAM, about
  30 KB of faad2's state for stereo HE-AAC.

## Memory

faad2 keeps the AAC element state and the SBR QMF buffers on the stack: the
deepest call chain (HE-AAC stereo) needs 57.8 KB. `DECODER_STACK` in
`src/audio/AudiobookService.cpp` is sized from that measurement. Decoder state
is allocated by `NeAACDecOpen()`, `NeAACDecInit2()` and the first frames: about
120 KB for stereo HE-AAC, mostly SBR state. Upstream declares a few tables
without `const`, so 1,378 bytes of them sit in internal RAM.

## Local changes

Two patches. In `libfaad/common.h`, so that `DISABLE_PS` can turn off
parametric stereo while keeping high-quality SBR:

```diff
-# ifndef SBR_LOW_POWER
+# if !defined(SBR_LOW_POWER) && !defined(DISABLE_PS)
 #  define PS_DEC
 # endif // SBR_LOW_POWER
```

In `libfaad/common.c`, so that `config.h` can choose the allocator:

```diff
 #if 0 // defined(_WIN32) && !defined(_WIN32_WCE)
     return _aligned_malloc(size, 16);
+#elif defined(FAAD_MALLOC)
+    return FAAD_MALLOC(size);
 #else   // #ifdef 0
     return malloc(size);
```

Only the `metalio_eink4-audiobook` environments build this library; the other
environments list it in `lib_ignore`. Do not reformat these files.

To update, copy the files listed above from a newer release, apply the
patches again, and change the version here, in `config.h` and in `library.json`.
