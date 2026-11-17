# Helix MP3 and AAC decoders

RealNetworks Helix fixed-point decoders, copied unmodified from
[ESP8266Audio](https://github.com/earlephilhower/ESP8266Audio) commit
`0c7c67a904c870e2dffc14504f9ad0d0b3641ea6` (`src/libhelix-mp3`,
`src/libhelix-aac`). Only `libhelix-mp3/player.h` was left out.
ESP8266Audio ships the license texts in `libhelix-mp3` only; the copies in
`libhelix-aac` are the same files.

Each file is under the RealNetworks Public Source License (RPSL), or the
Community Source License (RCSL) for RCSL licensees. The FSF lists the RPSL as
incompatible with the GPL, and the combined CrossPoint firmware is GPLv3 (see
`licenses/README.md`). Builds that include this library are for personal use
only and must not be distributed.

Only the local `metalio_eink4-audiobook` environment builds it; no CI or
release workflow builds firmware with it. Host tests compile it and
publish nothing. Do not reformat these files.
