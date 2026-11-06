# Full-colour (and animated) emoji: the font patch

Superseded by native emoji (docs/NATIVE-EMOJI.md): the from-source firmware draws emoji sent as UTF-8, with any
font, and that is the only emoji path the studio supports. This page keeps the firmware analysis and the older
bootloader patch (`make PATCH=1`), which drew colour cells of special fonts. The studio no longer makes those
fonts.

## Status (2026-10-31)

The patch is built and verified in emulation. It has not yet run on the sign.

- Patch code: `bootloader/patch/` (`hook.S` + `emoji.c`, 604 bytes, linked at 0xC60000).
  - It detours `font_draw_char`. Codes 0x80..0xFF of a font that loaded with alpha (image format 9) are drawn in
    their own colours: opaque pixels copied, alpha 0 skipped, partial alpha blended.
  - Everything else uses the original code.
- Delivery: `tools/mkpatch.py` builds a `PATCH` archive entry. `make PATCH=1 image` puts it in the flash
  image, and the bootloader applies it in RAM after the final MMU set-up.
  - It is all-or-nothing: three word patches, each checked against its expected original first.
  - The firmware on the chip is never modified.
  - The three patches:
    - `0x42DB0`: detour to the hook;
    - `0x44BC0`: font_load asks for alpha;
    - `0x00010`: heap start moved past the code; the heap now starts at 0xC61000.
- Verified:
  - `make emojitest` (`test/test_colour_emoji.py`) runs the patch inside the real firmware: its own PNG
    conversion, `font_build` and `font_draw_text`.
    - Mono fonts draw exactly as unpatched.
    - Letters of a colour font draw exactly as unpatched.
    - Emoji match an independent pixel model, and come out as silhouettes without the patch.
  - `test/patchsim.py` boots the PATCH=1 image on both memory paths in the co-simulator. The 3 words and the
    code are in place, all other RAM equals a normal boot, and one wrong expected word means nothing is applied.
  - `make cosim` still shows the default build identical to the original.
- Studio: no longer supported. The studio used to build colour emoji fonts (`fonteNN.png` in RGBA with a
  `.emoji.json` sidecar) for this patch. That feature is removed: emoji now go only through native emoji
  (docs/NATIVE-EMOJI.md), which needs no special font.
- Not done yet:
  - P3, animated emoji;
  - checking whether the scrolling-message path uses `font_draw_text` (fixed advance) or a proportional routine;
  - a test on the real sign.

## Firmware analysis

Reverse-engineered 2026-10-31 from ELITE_RGB V4.20. Every name below is in `analysis/symbols.tsv`
and in `analysis/ghidra_out/elite_rgb_named.c`.

### How text reaches the panel in stock firmware

1. `font_load` (0x44AA8) loads `fontXNN.png` with `image_load_png_file(..., alpha = 0)`.
   - The 5th character picks the layout: `e` = 16x16 grid (all 256 codes), `a` = 16x8 grid, `n` = digit strip.
   - The PNG becomes a colour RGB555 image (`image.format = 1`), through LED colour-correction tables.
2. `font_build` (0x448D4) stores the image in a 0x718-byte font record and measures every glyph
   (proportional-spacing tables).
3. `font_draw_text` (0x43424) calls `font_draw_char` (0x42DB0) per character.
   - `font_glyph_locate` (0x444C8) turns the code into a cell; there is no printable-ASCII filter.
   - `glyph_blit` (0x3EBF0) then draws the cell.
4. `glyph_blit` uses the glyph only as a mask, in all of its modes:
   - two colours, FG only, an RGB texture, or a palette texture (FLC effect frames);
   - mode 8 composites an FG layer with a BG layer.
   - No mode copies the glyph's own pixels, so colour glyph art is loaded in colour but drawn as a silhouette.

### Building blocks in the stock firmware

| Component | Address | Use for emoji |
|---|---|---|
| Image formats | `image_alloc` 0x44F9C | 1 = RGB555, **9 = RGB555 + alpha**, 2 = indexed + palette |
| PNG with alpha | `png_rows_to_rgb555a` 0x46360 | RGBA PNG -> format 9 (alpha 255 = opaque RGB555; else RGB444 + 3-bit alpha) |
| Colour copy | `image_blit_copy` 0x3E7FC | copies an RGB555 rectangle unchanged |
| **Alpha blend** | `image_blit_alpha` 0x3E894 | blends a format-9 rectangle over the frame. **Present but never called in V4.20** |
| Generic image draw | `image_blit` 0x3E4D0 | handles formats 1 and 2; format 9 is accepted but draws nothing |
| FLC decoders | 0x413CC (8-bit), 0x415D4 (16-bit) | animations already play in colour |
| Frame counter | `ledpanel_frame_count` | a time base for animating emoji |

The existing fonts are all 1-bit palette PNGs without alpha (fonte01..08; cells 7x10 up to 20x32).
Any change keyed on "font has an alpha channel" therefore leaves them untouched. On an 80x16 panel an inline
emoji matches `fonte04` (11x16 cells), or two cells for 22x16.

### Patch plan

- P1, colour fonts. In `font_load`, ask for alpha (one instruction: `li r5,0` -> `li r5,1`). A font PNG
  with an alpha channel then loads as format 9; the existing 1-bit fonts load exactly as before.
- P2, colour glyph drawing. In `font_draw_char`: if the font image is format 9, draw the cell with a colour
  kernel instead of `glyph_blit`.
  - Use a small new kernel rather than `image_blit_alpha`: alpha 0 skipped (truly transparent), alpha 255
    copied, partial alpha blended. `image_blit_alpha` never goes below 3/31 opacity, so "transparent" pixels
    would still tint the background.
  - Colour emoji then sit inline in normal text, scroll with it, and keep their own colours.
- P3, animated emoji. A colour font may stack F frames of the 16x16 grid vertically. The cell row is offset
  by `(ledpanel_frame_count / rate) % F`. An animated GIF becomes those F frames.
- Where the new code lives: the original firmware stays untouched on flash. Our C bootloader already
  decompresses ELITE_RGB into SDRAM, so it can also:
  - load a small `PATCH` archive entry (the new code) into unused high SDRAM;
  - apply a list of `{address, expected old word, new word}` branch patches. Each patch is applied only if the
    old word matches, so a different firmware version is never mis-patched.
  - The MMU set-up already maps all SDRAM banks.
- Verification before any hardware test:
  1. Run the patched `font_draw_text` under Unicorn with a test colour font and compare the rendered frame
     pixel by pixel with the studio's preview.
  2. Run the existing fonts through the same path to prove they draw exactly as before.
  3. Run the full boot in the bootloader co-simulator.

## Animated GIF support (done 2026-10-31 for animations; emoji still to do)

- Converter: `studio/marquee_studio/gif2flc.py` (also runnable as `tools/gif2flc.py`) turns GIF/APNG/WebP into an FLC sized to the panel, 16-bit true colour
  (default) or 8-bit with one shared palette. Every frame is a full BRUN frame.
- Verified against the firmware: `tools/test_gif2flc.py` decodes the output with the sign's own chunk
  decoders (0x420FC / 0x42220) under Unicorn. Every pixel of every frame matches, in both depths.
- Studio: Animations tab -> "Import an animated GIF", with name, depth and fit. Previews play at the sign's
  real timing.
- Player facts:
  - The sign runs at 40 fps (25 ms display frames). An animation frame is held for a whole number of them, from
    the threshold table 38/63/88... ms (`sign_frame_ms` in `studio/core.py` matches the firmware for every speed
    from 1 to 1100 ms).
  - 16-bit FLC is supported natively (image format 1, pixels used raw), but its BRUN is byte-wise with a
    byte swap, not the standard pixel-wise 16-bit BRUN.

### Original plan

- Windows, backgrounds, attract and effect animations (no firmware change): GIF and FLC are both
  palette-based, so conversion is mostly repacking:
  - per-frame palettes become FLC colour chunks;
  - frames become BRUN/SS2 chunks;
  - GIF's per-frame delays map onto FLC's single frame rate by repeating frames.
  The studio can convert, preview (with the decoder already validated against the sign's content) and pack them.
- Animated inline emoji: via P3 above.

## Open questions

- Font layout: do `font_glyph_locate` and the metrics in `font_build` behave on format-9 pixels? Measuring
  treats any non-zero pixel as ink, which suits emoji art.
- Unused high SDRAM: where it is depends on the board's SDRAM size and RTEMS's workspace. Find it from the
  app's memory set-up before choosing the patch address.
- Colour correction: format 9 skips the LED correction tables that format 1 uses. Emoji colours should be
  pre-corrected in the studio, or the kernel should apply the tables.
