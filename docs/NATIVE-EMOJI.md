# Native emoji

The from-source firmware can show emoji as emoji: a message carries them as UTF-8, the sign draws them in colour
from one shared sheet, and they work with every font. Plain text keeps working exactly as on the stock firmware.

This needs the from-source firmware with its extensions (`ELITE_RGB-ext`). The stock firmware cannot draw emoji.
Native emoji are the studio's only emoji path; the older colour-font patch (docs/COLOUR-EMOJI.md) is no longer
supported by the studio.

## How it works

### Content

One file, `emoji.bin`, made by `studio/marquee_studio/native_emoji.py` and packed as small as possible. The
firmware reads it from the content store in main flash when the store has one (see below), else from the boot
flash:
- a 20-byte header: magic `EMJ2`, cell size, count, palette size, frames, map length, unpacked length;
- one zlib stream holding the palette (RGB555, entry 0 transparent), the map (one emoji per line, UTF-8, without
  U+FE0F, in cell order), the cell table (per cell, 3 big-endian 16-bit words: first frame, frames, ms per frame)
  and the pixels (one palette index per pixel, frame after frame; a cell's frames are consecutive).

An `EMJ1` file (no cell table, one frame per cell) still loads.

The 329 emoji of `firmware/emoji/set.txt` at 16 px take 15.8 KB, against 37.5 KB as an RGBA PNG. The boot flash
keeps 45 KB free.

The pictures are pixel art drawn at 16 x 16 (and 32 x 32), so a 16 px panel shows them exactly as drawn: JOI3's
[Flat Pixel Art Emojis](https://joi3.itch.io/flat-pixel-art-emojis) (2,075 emoji, bought). The pack's files may not
be redistributed, so they stay on the owner's machine (`local/emoji-pack/`, git-ignored, or `MARQUEE_EMOJI_PACK`).
The repository carries the built set, `firmware/emoji/16/emoji.bin`, like a game carries its sprite sheet. The
studio builds from the pack when it is there and from the committed set otherwise.
- Change the set: edit `firmware/emoji/set.txt`, then `(cd studio && ../venv/bin/python -m
  marquee_studio.native_emoji update)` with the pack in place.
- Set size: faces, hearts and hands (the pack's first groups), then a few dozen common objects and symbols.

### Firmware

`firmware/ext/native_emoji.c` takes over four of the firmware's text routines, `text_width`,
`text_draw_proportional`, `text_draw_proportional_plain` (used by the display loop) and
`text_draw_proportional_clip_maybe` (used by message slots), and `glyph_blit_effect` for message lines. A
message line (attract lines, and record text too wide for its window) is drawn into a text image that reaches the
panel only as the mask of `glyph_blit_effect`, which paints the line's effect word: emoji drawn into that image
(it is 0x1414 wide) carry bit 15 as a tag, and the taken-over `glyph_blit_effect` draws the tagged pixels in their
own colours after the fill. Nothing else carries the tag, so every other blit draws as before.
- A text with no emoji from the map goes straight to the original routine, so every existing text, font and effect
  draws byte for byte as before.
- An emoji is recognised only as a UTF-8 sequence that is in the map. These start with a byte from 0xC2 and continue
  with bytes 0x80 to 0xBF; the U+1F300-and-up emoji all start `F0 9F`, and 0x9F is a control code that sign text
  in Latin-1 does not contain. The longest map entry wins, so joined (ZWJ) sequences work, and a following U+FE0F is
  skipped.
- An emoji counts as one proportional glyph, as wide as a sheet cell, with the same separator columns as a letter.
  So layout, centring and scrolling treat it like any other character.
- It is drawn 1:1, centred vertically on the text line. With a font shorter than the panel it reaches above and
  below the line, and the destination clips it.
- `emoji.bin` is unpacked on first use, with the firmware's own zlib (`uncompress`) into a `malloc`ed buffer
  (palette, map and pixels: about 86 KB of RAM for 329 emoji). If the file is missing or bad, nothing changes: no
  emoji is ever matched.
- Palette index 0 is transparent; every other pixel is written as its RGB555 colour.

### How the code gets into the firmware

`analysis/tools/fwext.py` adds `firmware/ext/*.c` to the from-source image without moving anything:
- The extension code (about 3.6 KB) is linked as one block for RAM, just past the C heap's old start. The heap moves
  up 12 KB (the word at 0x10) to make room, and the block begins with 256 bytes of extension state.
- The block's bytes are stored in the image's free gaps, the space the compiled C left in replaced functions (38
  gaps), with a chunk table.
- A 124-byte loader takes over `boot_card`, the app's first C function. It copies the block into RAM, writes the
  data cache back, invalidates the instruction cache, and continues at `ext_boot_card`, which clears the state and
  runs the original `boot_card`.
- Each `ext_<name>` takes over `<name>`: its first instruction becomes a branch to the extension, and
  `<name>__orig` runs the displaced instruction and continues in the original.
- Every other byte of the image is checked to equal the stock-identical build.

## Studio

Settings, "Sign firmware", "Native emoji" (off by default). With it on:
- emoji are sent as UTF-8 in RS-485 records and message lines (`native_emoji.encode`); other characters stay
  Latin-1;
- the preview draws them with the firmware's rule (`native_emoji.parse`, `layout`, `pixels`);
- the emoji picker lists only the emoji in the sign's set, and the compose page warns about any other emoji (it
  would be sent as a question mark);
- a content pack includes `emoji.bin`.

## Verification

- `umami-marquee-sim-test native-emoji` (formerly `sim/test_native_emoji.py`) boots the full boot-flash image in the system
  simulator, then calls the text routines inside the running firmware:
  - ASCII and Latin-1 text: the same widths as the original routines, and the sheet is not loaded for ASCII;
  - the first emoji unpacks `emoji.bin` from the boot flash: the palette, map and pixels in RAM equal the file's;
  - "A🍒B" with all 9 loaded fonts (8 to 32 px) and all three drawers: the letters equal the original routine
    drawing them alone, every emoji pixel equals an independent model of the 1:1 rule (including the clipping
    above and below short fonts), and the widths agree;
  - the studio's preview of the same text matches the firmware's width and emoji pixels for each of the sign's
    fonts.
- With no emoji in play, the extended firmware matches the stock firmware in the system simulator, as the plain
  from-source build does (these comparisons now run as `umami-marquee-sim-test compare`, `compare-rs485`, `compare-db9`
  and `compare-menu`, with the scripts' arguments):
  - `sim/compare.py`, long run (450 M / 600 M instructions): 92 of 95 distinct frames identical. The other 3 are the
    start-up progress bar caught at another length: they differ only in panel rows 12 to 15, as with the plain
    build (docs/REBUILD-PLAN.md 4f);
  - `compare_rs485.py`: the same handling, jackpot records and NVRAM; `compare_db9.py`: the same replies;
  - `compare_menu.py`, navigation and an edit saved to NVRAM: the same display states, the same settled screens,
    identical NVRAM.

  The menu comparison first failed for both from-source builds: the simulated remote sent keys a fixed number of
  frames apart, and the app treats the same code within 190 ms as one held key, so how many presses merged depended
  on drawing speed. Keys are now at least 300 ms apart (`appsim.ir_press`), and screens are compared once they
  settle, since the menu's slide and cursor blink run on the tick clock.
- `studio/test_studio_api.py` covers the setting, the preview, the catalogue, message-line encoding and the content
  pack.

Not yet run on the sign.

## Building

```sh
(cd analysis/tools && ../../venv/bin/python rebuild.py --trials 24 && ../../venv/bin/python fwbuild.py \
    && ../../venv/bin/python fwext.py)
make -C bootloader fwimage PYTHON=../venv/bin/python        # NATIVE_EMOJI=0 for the plain stock-equivalent image
build/simulator/umami-marquee-sim-test native-emoji             # cmake --build build/simulator first (docs/SIMULATOR-GUI.md)
```

## Your own emoji, animated

In the Studio's emoji picker, the **Custom** tab has **Import GIF**. It takes a GIF (or PNG, APNG, WebP) and makes it
an emoji of the sign with the next code point of Unicode's private use area (U+E000, U+E001, ...). It can then be
used in any text, like any other emoji: Compose, Lines, and the apps' settings. **Remove this emoji** takes it out
again.

- **The picture.** Pixel art that fits the cell is scaled up by the largest whole number that fits. A bigger picture
  is shrunk to fit and keeps its shape. Either way it is centred in the cell, which is as tall as the panel. The
  picker shows the result at the sign's size.
- **The timing.** The shortest frame of the GIF sets the frame time, at least 50 ms. A longer frame repeats, so each
  frame lasts about as long as in the GIF. An emoji has at most 64 frames; a longer GIF is slowed down to fit.
- **Where it lives.** The imported files are in the Studio's content folder, under `emoji/` (`custom.json` and the
  originals). The boot flash keeps the stock set only: `make fwimage` builds it from `set.txt`. The full set, with
  your emoji, goes to the sign in the content store, with the apps. **Upload apps** on the Apps page sends
  `emoji.bin` whenever there is an imported emoji, and the firmware reads it on its next emoji.

### How the firmware animates them

A message line's text is drawn once into its text image. After that it is only blitted through the line's effect.
So an animated cell is put right at blit time:
1. `emoji_draw` notes each animated cell it draws into a text image: image, x, y and cell, in a ring of 32.
2. For each noted cell that the source still holds at that place, as frame 0, `ext_glyph_blit_effect`:
   - takes the cell out of the mask, so the line's background shows where the current frame is transparent;
   - blits;
   - puts frame 0 back;
   - draws frame `(t * 25 / ms) % frames` in its own colours.

`t` is the step count of the line on screen, or the app's own `t` (`emoji_anim_time`, from apps.c); one step is 25 ms.
The Studio's emulation (`sign-emu`, `native.ts` `animatedBlit`) does the same, with one ring per text image.

`frontend/packages/sign-emu/test/appvm_firmware.test.ts` checks this against the firmware in the simulator: its
scenario imports a five-frame GIF emoji, and the Text app scrolls it. Every drawn frame must be identical. The
message-line path uses the same drawing and blit code with the line's step count; it has no firmware test of its
own yet.

