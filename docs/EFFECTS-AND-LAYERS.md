# How text, effects and layers work on the sign (2026-10-31)

This note describes how ELITE_RGB composes a message, as far as it is currently understood, and how Marquee
Studio reproduces it. Each claim is labelled with what supports it:
- [decompile]: read from the Ghidra output (`analysis/ghidra_out/elite_rgb_full.c`).
- [data]: checked against the bytes in the dump.
- [inferred]: the best current reading, not yet checked on the sign.
- [firmware run]: checked by running the firmware's own code under Unicorn (studio/test_effects.py).

## 1. Correction: the "52 colour effects" are diagnostic test patterns

Earlier notes (and the first studio build) wrongly said that `FUN_0000ef48` is the text compositor, with 52 colour
effects for text.
- `FUN_0000ef48` is the handler for the DIAGNOSTICS menu entry [decompile].
  - At line ~21120, the menu record has the label `"DIAGNOSTICS:"` and the handler `FUN_00011394`.
  - `FUN_00011394(0)` calls `FUN_0000ef48`, and `FUN_00011394(1)` calls `FUN_0000e140`.
- `FUN_0000ef48` loops forever. Each pass it clears the panel, draws pattern number `iVar16` (0x00–0x33, which is 52
  patterns) over the full panel, and polls a key [decompile]:
  - `]` (0x5D): next pattern. After 0x33 it wraps to 0x00.
  - `[` (0x5B): previous pattern.
  - `}` / `{` (0x7D / 0x7B): raise or lower the adjustable level, on patterns 0x13–0x14 and 0x32–0x33.
  - `1`–`9`: set that level directly, on patterns 0x13–0x14.
  - `-` (0x2D): exit.
- Pattern families [decompile]:
  - 0x00–0x0E: full-panel solid red, green or yellow at several intensities.
  - 0x0F–0x12: column gradients, and red/green/blue horizontal thirds.
  - 0x13–0x14: user-adjustable RGB fill with a numeric readout.
  - 0x15–0x1D: sweeping single columns, 5-px bars and horizontal lines.
  - 0x1E–0x33: further sweeps, wipes and counters.

  These are LED, driver and wiring checks, not text effects.
- Practical use [inferred]: this mode is a good way to confirm the panel width and spot dead pixels.
  Enter DIAGNOSTICS from the on-sign menu, then step with `[` / `]`. The column sweeps show every column.
  The key source (`FUN_00059bc4`) is still to be traced, so it is not yet known whether it is the serial console
  or the front keypad.

`studio/effects.py` still holds approximations of these patterns. They were drawn as text fills, which is not
what the sign does. The studio no longer offers them as a text option.

## 2. The parameters of a text item: FG, BG, outline

Every text item has the same parameter group. The items are Label, Odometer (the jackpot value), Win, Location and
New Jackpot, for each pool [decompile] (`FUN_0002def8`, strings at 0xA067C…):

| Theme key | Meaning |
|---|---|
| `Pool,%d,<Item> Effect FG` | the glyph **fill**: the foreground layer |
| `Pool,%d,<Item> Effect BG` | what is drawn **behind** the text: the background layer |
| `Pool,%d,<Item> Effect Outline Style` | outline shape (index into the table at 0xB5510) |
| `Pool,%d,<Item> Effect Outline Colour` | outline colour (index into the table at 0xB5528) |

FG and BG are stored together as one 32-bit word, `FG << 16 | BG` [decompile]:
- `FUN_0001ec60` packs the word, and `FUN_0001e6e8` unpacks it.
- The word lives at `0xE5D28 + pool*0xFC`.
- The outline style and colour live at `+0x70` and `+0x74` in the same pool record.

### The 16-bit FG/BG codes

The firmware builds two pick-lists at start-up in `FUN_0001e51c` [decompile]:
- The FG list (up to 0x6B entries) goes to `0xE59BC`.
- The BG list goes to `0xE5A92`.

The theme values are matched against these lists. Only solid colours, `0x8000`, `0x8001` and the effect slots
can be selected (`effect_code_valid` 0x40F4C); the other codes below exist only in the drawing code.

| Code | Meaning | Source |
|---|---|---|
| bit 15 clear | **solid RGB555 colour**. FG list: `7C00 03E0 7FE0 0000 0400 0020 0420` (red, green, yellow, black, then 3 near-black values). BG list: the same, but starting with black. | [data] tables at 0x9EDB8 and 0x9EDC8 |
| `0x8000` | **as BG: transparent** (pixels are left alone). **As FG it is not transparent:** `effect_resolve` (0x40D10) turns it into 0xFFFF, and the blit paints that on the glyph pixels. `odometer_digit_draw` returns early on FG 0x8000, so the firmware's authors avoided it. | [firmware run] |
| `0x8001` | **red → yellow → green downward wipe**, 1 px wide and h high (h = the window height, or the font cell height for per-character draws). t = frame_count >> 2: rows 0..(t mod h) take the new colour, so one row per 4 frames, and the colour moves on every h rows ((t / h) mod 3). A full cycle is 12h frames. The only code anchored to the blit's top-left, not to the panel. | [firmware run] `effect_texture_8001` 0x408FC |
| `0x8002` | diagonal red/green/yellow stripes, each 4 px wide, in a 12×12 tile: pixel = s[((x + y − (frame_count >> 1)) mod 2^32) mod 12]. Because of the unsigned index, the pattern is shifted by 4 for the first 2(x+y) frames after power-up. **Not selectable** on the sign; only drawn. | [firmware run] `effect_texture_8002` 0x40AB8 |
| `0x8003`..`0xBFFD` | nothing (`effect_resolve` returns 0): only the outline is drawn. If the FG half fails, the BG half is never resolved. | [firmware run] |
| `0xBFFE`, `0xBFFF` | red/green/yellow palette cycle, changing every 16 frames. For text items the two look **identical**: phase is always 0, and the codes differ only in adding or subtracting it. Not selectable. | [firmware run] `effect_palette_cycle` 0x408A8 |
| `0xC000 \| n` | **effect animation slot n**. n ≥ 100, or a slot whose file did not load, draws **solid yellow 0x7FE0** over the whole window. | [firmware run] |

At boot, `FUN_000406dc` loads slot n from the flash file `effectNN.flc` (`effect00.flc` … `effect99.flc`) into
`0x188098[n]` [decompile]. The boot flash holds slots 00–19 [data]. Their authoring names are in each FLC's
prefix chunk. Examples: `DMOND-BG` (diamond), `GLT2-BG` / `GLTR-*` (glitter), `RBOW-*` (rainbow), `SHDE-*` (shade),
`WAVE-*` (wave) and `BLUE`. The letter suffix is the colour mix, for example BG = blue-green, RYG = red-yellow-green.

So the same effect animation can be used as the fill inside the letters (FG) or as the background (BG).

## 3. Layer order and timing (verified by running the firmware's drawing code)

`display_main_loop` draws a pool text item in this order, back to front:
1. Background: `glyph_blit_effect` of an empty mask with the whole FG/BG word, so the BG fills the window.
   Some items also have a separate window FLC behind them, for example the odometer's `valueWindow.flc` /
   `RNGdraw.flc`.
2. Outline: `glyph_outline_blit`, run by run.
   - Styles (enum 0xB5510): None, Thick (the 8 neighbours), Thin (the 4 neighbours), Drop Shadow (offset +1, +1).
   - Colours (0xB5528): Black, Red, Green, Yellow, Blue, Cyan, Magenta, White. Boards with display_config bit 14
     clear offer only the first 4.
   - The colour index sits in the upper 16 bits of the outline word.
   - Rows are clipped to the clip rect, but columns are clipped only at its left and right edge. Thick and Thin
     skip rows with y < 0; Drop Shadow does not.
3. Foreground: `glyph_blit_effect` again, with BG forced to 0x8000, so only the glyph pixels are painted.

### How textures are anchored

- In `glyph_blit_effect`, FG and BG textures are fixed to the panel: panel pixel (X, Y) shows
  tex[Y mod h][X mod w]. Scrolling text moves *through* the pattern. 0x8001 is the exception (fixed to the blit).
- `font_draw_char_effect` anchors FG and BG separately at their own origins. The subtraction is unsigned, so a
  negative origin jumps on tiles whose size is not a power of two.

### How fast effect animations run

- A slot ticks at most once per panel frame (40 frames/s), however many glyphs use it. The stamp per slot is
  `effect_slot_tick_stamp` (0x192930).
- `flc_tick` shows each FLC frame for 1 + `flc_speed_to_hold(speed)` panel frames: an FLC speed of 71 ms gives 3
  frames, 42 ms gives 2, 28 ms gives 1.
- After the header's last frame the player rewinds; the FLC ring frame is never shown. One loop is exactly
  header frames × (hold + 1) panel frames.
- A slot that was not ticked for more than 8 frames restarts from frame 0.

## 4. How Umami Marquee Studio maps this

- Pixel-exact: `studio/core.render_message555` draws one frame per 40 fps panel frame through the emulator's
  port of this code (`studio/marquee_studio/sign_emu.py`: `Effects`, `EffectSlot`, `outline_blit`, `draw_text_item`).
  `studio/test_effects.py` (539 checks, run by `./check.sh`) compares it frame by frame with the firmware's own
  drawing code under Unicorn: the texture builders, pool text items for 13 FG × 11 BG codes, timing, anchoring,
  every outline style and colour, and per-character draws.
- Design tab:
  - Text colour / Background colour: the solid FG / BG, used when no effect is chosen.
  - Foreground / Background effect: any `effectNN.flc` (code `0xC000|n`), shown with its authoring name, plus
    Transparent and the R/Y/G wipe (0x8001).
  - Outline: style (Thick / Thin / Drop Shadow) and one of the sign's 8 colours.
  - Firmware code: read-only; the packed `FG<<16|BG` word, for example glitter (effect03) on black is
    `0xc0030000`.
- Render spec keys: `colour` / `foreground`, `bg_colour` / `background`, `outline`, `outline_style`, and
  `frame0` (the starting frame count, default 1000). `core.effect_code` also accepts "Transparent",
  "Red/green/yellow wipe" and hex codes.
- Previews play at the sign's 40 frames/s.
- Effect library tab: every effect slot twice, as an FG fill and as a BG layer.

## 5. Open questions

- Which input feeds the DIAGNOSTICS keys.
- Windows that go partly off the panel: `glyph_blit_effect` aliases its rect and position there, and the studio
  port does not copy that. Only windows inside the panel are tested.
