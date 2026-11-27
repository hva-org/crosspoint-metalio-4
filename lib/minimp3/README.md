# minimp3 MP3 decoder

[minimp3](https://github.com/lieff/minimp3) by lieff, copied from commit
`48e3a085f0e1a99446b34614e1e7f17f09dc2e82`. Only `minimp3.h` and `LICENSE`
are kept; `minimp3_ex.h` (file and seek helpers), the player, tests and
fuzzers are left out.

`minimp3.h` is unmodified. `src/minimp3.c` is local: it compiles the
header-only decoder once with `MINIMP3_ONLY_MP3` (the readers pass Layer III
frames only) and `MINIMP3_NO_SIMD` (Xtensa has no SSE or NEON; host tests use
the same portable C path as the device).

License: CC0-1.0 (public domain dedication), see `LICENSE`.

Only the `metalio_eink4-audiobook` environments build this library; the other
environments list it in `lib_ignore`. Do not reformat these files.

To update, copy `minimp3.h` and `LICENSE` from a newer commit and change the
commit above.
