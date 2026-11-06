# Message line markup (ELITE_RGB V4.20)

Message lines are what the sign scrolls: attract text and controller messages. There are 17 lines of 256 bytes
each (`MESSAGE_TABLE` in NVRAM; `message_table_ptr`). Each line is plain text with ESC (0x1B) commands mixed in.
`message_text_next_token` (0x46C78) reads them, and `textline_step` (0x469D0) animates each piece.

This is verified against the firmware itself: `analysis/tools/test_message_markup.py` runs the real tokenizer under Unicorn
on hand-built lines and checks every row in the table below. Run it with `./check.sh`.

## Segments

A line is shown as a series of segments. The firmware collects text bytes up to the next CR (0x0D), ESC or
NUL. That run is drawn once into a text image and then animated:
1. it slides in (entry);
2. it holds;
3. it slides out (exit);
4. the next segment is read.

- CR ends a segment. The CR itself stays as the first byte of the next segment and is drawn with that font's
  glyph 0x0D (normally blank).
- NUL ends the line. If the line was included from another one (see `ESC 0x80`), reading carries on in that
  outer line. Otherwise the text line finishes (state 4).
- The read position is kept per line (`message_line_state[n] + 0x114`). The next message on that line carries on
  where the last one stopped.

## Commands

Arguments are bytes, and colours are big-endian RGB555 words. A command applies to the segments after it.

| bytes | effect |
|---|---|
| `1B 30 hi lo` (`ESC '0'`) | text colour (fg) |
| `1B 31 hi lo` (`ESC '1'`) | background colour (bg) |
| `1B 40 speed` (`ESC '@'`) | entry from the right (the default) |
| `1B 41 speed` (`ESC 'A'`) | entry from the left |
| `1B 42 speed` (`ESC 'B'`) | entry from the bottom |
| `1B 43 speed` (`ESC 'C'`) | entry from the top |
| `1B 44 speed` (`ESC 'D'`) | entry from the top with the text image at x = 0 instead of centred (sets a flag, see "Quirks") |
| `1B 50 hold speed` (`ESC 'P'`) | after the entry, hold `hold × 4` frames, then exit to the left |
| `1B 51 hold speed` (`ESC 'Q'`) | … exit to the right |
| `1B 52 hold speed` (`ESC 'R'`) | … exit upwards |
| `1B 53 hold speed` (`ESC 'S'`) | … exit downwards |
| `1B 54 hold speed` (`ESC 'T'`) | … exit downwards; = `ESC 'S'` plus the same flag as `ESC 'D'` |
| ``1B 60 n`` (`` ESC '`' ``) | font n, left-aligned at the box's x; n = 0xFF: centred, with a font picked per segment (see below). Text is always drawn proportionally. n is the load order of the fonteNN.png files found (fonts_load), not NN: with the original 8 fonts, font 0 is fonte01.png |
| `1B 80 n` | include line n here, then come back (nested, up to 18 deep) |

Any other command byte is skipped, along with the ESC.

## Per-character effects (our firmware)

With our firmware (firmware/ext/native_emoji.c), a text may carry **in-text effect commands**. They start with the
in-text escape 0x1F (ASCII US), take no room, and apply to the characters after them. Their arguments are
printable, so a command never contains NUL, CR or ESC:

| bytes | effect |
|---|---|
| `1F 'm' d` | motion d: `0` none, `1` wave (each column rides a moving wave, ±2 px), `2` bounce (each letter hops up to 3 px in turn), `3` shake (each letter jitters a pixel), `4` blink |
| `1F 'f' hhhh` | fill: the letters' own effect word, 4 hex digits: an RGB555 colour, `8001` wipe, `8002` stripes, `C0nn` effect animation slot nn |
| `1F 'f' '-'` | fill: the line's own again |
| `1F 'o' hh hh` | on / off time in tenths of a second (hex): the effects run while on and rest while off; for blink it is the rhythm (default 0.3 s / 0.3 s). `o0000`: always on |

A 0x1F followed by anything else is skipped on its own. Unlike ESC commands, which end the segment, these sit
inside a segment, so one segment mixes effects letter by letter; the stock tokenizer passes them through as text
bytes. The text is drawn once; each step, the moving characters' columns are displaced and the line is blitted in
runs of columns of one effect word, so the effects combine with the background and with animated emoji. The
Studio's Compose sets them per selected letters (state/composer.tsx withEffects); sign-emu (native.ts fxOffset,
motionBlit) draws the same frames, checked against the firmware in the simulator (test/firmware.test.ts, attract
line 3).

## How the slides behave

The entry slide starts with the segment just outside the panel, on the chosen side. Where it ends depends on the
hold:
- hold = 0 (no exit command yet): the segment slides all the way across and out the far side, like a classic
  scrolling marquee. Then the next segment starts.
- hold > 0: the segment stops centred, holds, then leaves by the exit direction.
- The hold is used up. `textline_step` counts it down past 0 and leaves it at 0xFFFFFFFF, and only `ESC P..T` sets
  it again. A later segment without its own exit command would hold for 2^32 frames (about 3.4 years). Give every
  held segment its own `ESC P..T`, or `ESC P 00 speed` to go back to scrolling through.

Speeds are in pixels per second. `image_slide_init` computes `frames = sqrt(dx² + dy²) / speed × 40` (the sign
runs at 40 frames/s). Speed 40, the default from `textline_init`, is 1 pixel per frame, so it crosses an 80-pixel
panel in 2 s. Speed 80 is twice as fast.

## Example

```
1B 30 03 E0          green text
1B 41 08             enter from the left at speed 8
1B 50 0A 08          hold 40 frames, then leave to the left at speed 8
"JACKPOT!" 0D        segment 1
1B 30 7C 00          red text
1B 60 FF             centred (font picked from the segment's first character)
1B 50 0A 08          segment 2 needs its own hold + exit (the first hold was used up)
"$1,000"             segment 2
00
```

## Quirks (found by running the firmware, studio/test_markup.py)

- Centred mode picks the font per segment. `font_pick_for_text` (0x43F20) picks the largest font that fits,
  judged from the segment's *first character*. That is the CR when the segment starts with one. The box is the
  5140 x H text image for entries from the right/left, or the display for entries from the bottom/top. The picked
  font stays in place for later segments.
- Left-aligned text cannot stop centred or slide vertically. Those slides centre the whole 5140-pixel text
  image, which puts left-aligned text at about x = -2530, off the panel. Use centred text, or `ESC 'D'`.
- `ESC 'D'` / `ESC 'T'` set a flag that lives in a register (r24). `message_text_next_token` never initialises
  it, so a plain `ESC 'C'` entry depends on whatever the display task left there.
- A font taller than the line, left-aligned, hangs the display task. pos.y = (h - cell_h) >> 1 underflows
  (unsigned), and `draw_line` then steps a 16-bit y towards a 32-bit end it never reaches (loop at 0x45550).
- NUL bytes inside commands: red is `1B 30 7C 00`, and hold 0 is a 00 byte. The tokenizer reads them fine, but
  anything that copies the line as a C string would cut it there.

## Notes

- Where the lines come from: the attract lines set in the menu (`Textline,%d,Text`) and controller text messages
  (display state 6). This doc covers only how a stored line is drawn, not which protocol message writes it.
- The studio generates and previews these commands ("Message lines" tab; `core.encode_markup` /
  `decode_markup`). Its emulator port of the text-line code is pixel-identical to the firmware frame by frame.
  Checked by `studio/test_markup.py` against the real tokenizer and `textline_step`.
- Which text goes through this tokenizer (traced 2026-10-31):
  - Attract lines 0..15 (MESSAGE_TABLE): always. They are set from the menu (`Textline,%d,Text`), the theme,
    and `protocol_winner_text` (lines 0 and 1).
  - The winner text, through line 16: a pool's location text, as received in RS-485 records (Cougar
    `text[60]` with flag 0x20, and the other protocols' value texts). It goes through the tokenizer only when it
    is wider than its window in every font. `display_main_loop` first lets `font_pick_for_text` pick the largest
    font for the whole raw text, with the ESC bytes counted as glyphs. Only if even that is wider than the window
    does it copy the text into line 16 (`message_line_set`) and scroll it as markup. When it fits, it is drawn
    plainly (`text_draw_proportional_plain`), and ESC bytes come out as glyphs. The command bytes add width,
    so most marked-up texts do scroll on an 80 x 16 panel (the studio's default size), but a short one ("WIN") fits and would show junk.
    The studio decides this with the same rule (`core.record_text_scrolls`, checked against the firmware in
    `studio/test_markup.py`) and warns before sending.
  - Controller messages (CON2 0xF7, JWAM; display state 6) do not use it. They go to the
    TEXT_LINE_DATABASE and are drawn by the message-slot renderer (`message_slot_load` / `message_slot_render`),
    which has its own format.
