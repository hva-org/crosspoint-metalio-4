# Metalio E-Ink 4 Display Notes

These notes cover the GDEM0397T81 panel (SSD1677 controller) on the Metalio
E-Ink 4. They record what was tried against the sleep-screen "ink bleed", what
worked, and what failed, so the failed change is not tried again.

## The symptom

After sleep or power-off, the sleep screen first looks right. About 20–30 s
later the whole panel turns a mottled gray within a few seconds, and the screen
from before sleep shows through as an inverted ghost. It happens on battery and
on USB. It gets more likely the longer the device was used without sleeping.

## Fix in use: power the panel down after every refresh

Before this fix, Metalio left the panel powered between refreshes for as long
as the device was awake. FAST refreshes activate the board LUT with
`0x22 = 0xCC`, which leaves the panel's driving voltages on, and
`controllerIdle()` does nothing on SSD1677. The panel also stayed powered while
the screen was idle. When it was finally powered down at sleep, the charge
built up in the ink relaxed and washed the image gray.

`src/main.cpp` now forces the renderer's existing power-down path on Metalio
(`metalioPanelOffAfterRefresh()`). This is the same path as the X4/X3 "Sunlight
Fading Fix" setting, so every refresh ends with the panel powered down. The
vendor firmware's black/white refreshes power down the same way: `0xCF` for
partial, `0xC7` for full.

- **Result:** the bleed became much rarer and lighter. It still happens about
  1 sleep in 6–7, mostly after a long reading session without sleep.
- **Cost:** Metalio refreshes block instead of overlapping with rendering.

## Failed: removing the same-colour pulses from the page-turn LUT

`lut_fast_metalio` (`freeink-sdk/libs/display/FreeInkDisplay/src/lut/Ssd1677Luts.h`)
is the vendor's partial LUT (`kEpdLutPartial`,
`freeink-sdk/docs/metalio-waveform-source.md`). Pixels that change get 20
frames of drive. Pixels that do not change also get a short pulse toward their
own colour:
- black → black: row L0 = `0x05`, 2 frames toward black.
- white → white: row L3 = `0x08`, 1 frame toward white.

Those pulses always push the same way. The theory was that they build up
charge over a long session and cause the remaining bleed. The test set both
rows to `0x00`.

The hardware result was much worse:
- Ghosting appeared during normal use. Ink was blurry, and text and images from
  earlier screens overlapped the current one.
- A screen refresh did not clear it.
- The screen faded lighter and lighter while the device was in use.

The change was reverted and never merged.

**Why the pulses are needed:** CrossPoint's anti-aliasing and gray image passes
leave pixels gray that the next black/white page turn treats as unchanged.
The same-colour pulses are what pull those pixels back to black or white, and
they keep blacks deep between refreshes. The vendor's note on L0 points the
same way: its weaker alternative (`0x40`) is marked "black will appear
lighter". The black-pulse HALF clean uses
the same LUT, so it cannot make up for them. **Keep these pulses.**

## Open leads for the remaining bleed

- **The anti-aliasing gray pass** (`lut_grayscale_metalio`) pushes AA pixels one
  way only. It also runs with a different VCOM (`0x30`) from page turns
  (`0x10`) and from factory gray (`0x08`). Test it first without code: do a long
  reading session with Text Anti-Aliasing off, then sleep.
- **Balanced pulses:** any future LUT change must still settle AA pixels and
  keep blacks deep. Check for ghosting and fading during use in a short session
  before starting a long bleed test.

## Testing a change

- **Ghosting during use:** read 20–30 pages with AA on, including a page with
  an image. Check for leftover text, gray halos, and lighter blacks.
- **The bleed:** use the device normally, including long reading sessions, and
  count the sleeps that bleed. At about 1 in 6, it takes around 20 clean sleeps
  before a fix can be called confirmed.
