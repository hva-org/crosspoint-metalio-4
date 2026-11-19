#!/usr/bin/env bash
# Regenerates the small audio fixtures used by the audiobook reader tests.
# Requires ffmpeg with the aac and libmp3lame encoders. Output is committed,
# so the tests never need ffmpeg.
set -euo pipefail
cd "$(dirname "$0")"

common=(-hide_banner -loglevel error -y -bitexact)

# MPEG-2 Layer III CBR, 22.05 kHz mono, with ID3v2 and ID3v1 tags.
ffmpeg "${common[@]}" -f lavfi -i "sine=frequency=440:sample_rate=22050:duration=2" \
  -c:a libmp3lame -b:a 32k -metadata title="Tone CBR" -id3v2_version 3 -write_id3v1 1 tone_cbr.mp3

# MPEG-1 Layer III VBR, 44.1 kHz stereo; the Xing header carries a TOC.
# 1.5 s of silence, then 1.5 s of noise, so frame sizes jump at the midpoint
# and only the TOC can map a time to the right byte.
ffmpeg "${common[@]}" -f lavfi \
  -i "aevalsrc=exprs=gte(t\,1.5)*(random(0)-0.5)|gte(t\,1.5)*(random(1)-0.5):sample_rate=44100:duration=3" \
  -c:a libmp3lame -q:a 4 tone_vbr.mp3

# AAC-LC in MP4 with the moov box first.
ffmpeg "${common[@]}" -f lavfi -i "sine=frequency=440:sample_rate=22050:duration=2" \
  -c:a aac -b:a 32k -movflags +faststart tone.m4a

# M4B with three chapters and the moov box after mdat (ffmpeg's default).
cat > chapters.txt <<'EOF'
;FFMETADATA1
[CHAPTER]
TIMEBASE=1/1000
START=0
END=1000
title=Opening
[CHAPTER]
TIMEBASE=1/1000
START=1000
END=2000
title=Middle Part
[CHAPTER]
TIMEBASE=1/1000
START=2000
END=3000
title=The End
EOF
ffmpeg "${common[@]}" -f lavfi -i "sine=frequency=440:sample_rate=22050:duration=3" -i chapters.txt \
  -map 0:a -map_chapters 1 -c:a aac -b:a 32k -f ipod chapters.m4b
rm chapters.txt
