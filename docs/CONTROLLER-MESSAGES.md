# Controller messages (ELITE_RGB V4.20)

These are the messages a jackpot controller sends for display (Mikohn CON2 command 0xF7, JWAM). They are shown in
display state 6. They do not use the ESC markup of docs/MESSAGE-MARKUP.md; their own format is:
- stored in the NVRAM block `TEXT_LINE_DATABASE` (0x2000 bytes) as a byte stream;
- unpacked into message records (`text_line_db_unpack`, 0x6019C), 0x11F4 bytes each;
- laid out (`message_slot_layout`, 0x4D83C) and animated (`message_slot_animate`, 0x4E46C) by the message-slot
  renderer.

The stream format is verified by `analysis/tools/test_claims.py` (`controller_message_unpack` runs the real
unpacker). The command meanings below are read from the decompiled renderer and have not been run yet.

## Stored form (byte stream)

| bytes | meaning |
|---|---|
| first byte | kind (0x0F and 0xFF are treated specially) |
| `1C` + u32 | attribute for the characters that follow: an effect word, FG << 16 \| BG (docs: effect codes in ANALYSIS.md) |
| `1B` + u32 | command (stored as character 0 with this word) |
| `1C FF FF FF FF` / `1B 00 00 00 00` | a literal 0x1C / 0x1B character |
| any other byte | a character, drawn with the current attribute |

The starting attribute is 0xFFFEFFFE. Up to 256 characters plus commands. Words are big-endian.

## In NVRAM: the TEXT_LINE_DATABASE block (0x2000 bytes)

A chain of entries ending with a size of 0xFFFF. Each entry is `u16 size, u8 id, u8 kind`, followed by the
stream above (`text_line_db_entry_special`, 0x5FF14, walks it). Kind 0x0F or 0xFF marks a special entry.

## Record (unpacked, 0x11F4 bytes)

| offset | field |
|---|---|
| +0 | record number (set by the unpacker) |
| +8 | kind byte |
| +0xA | count (u16) |
| +0xC | chars[256] (0 = command) |
| +0x10C | attrs[256] (u32: the effect word, or the command word) |
| +0x508 | display rectangle (x, y, w, h) |
| +0xB84.. | renderer state (font, position, motion, hold, flash, background) |

## Commands (high 16 bits = opcode, low 16 bits = argument; b0 = high byte of the argument, b1 = low byte)

| opcode | argument | effect |
|---|---|---|
| 0x0001 | (ignored) | show the next text in place, no motion |
| 0x0002 | speed \| 0x8000? | move the text to the right edge of the rectangle |
| 0x0003 | speed \| 0x8000? | move until the text is fully off the left edge (marquee scroll) |
| 0x0004 | speed \| 0x8000? | move up, off the top |
| 0x0005 | speed \| 0x8000? | move down, off the bottom |
| 0x0100 | b0, b1 | alignment, b0 when the text fits and b1 when it does not: 5 left-top, 6 right-top, 7 centre-top, 0xD / 0xE / 0xF left / right / centre, vertically centred |
| 0x0200 | 0xFFFF | font: the largest that fits the rectangle (too wide in every font: the text "does not fit") |
| 0x0200 | b0 = 1, b1 = p | font whose height fits p % of the rectangle height |
| 0x0200 | b0 = 2, b1 = p | font whose width fits p % of the rectangle width |
| 0x0200 | b0 = 0, b1 = n | font n |
| 0x0300 | frames | hold after the move, in frames at 40 fps (default 40 = 1 s) |
| 0x0400 | b0 = on, b1 = off | flash: text visible for b0 frames, hidden for b1 frames (b1 = 0: no flashing) |
| 0x0600 | effect | background: the rectangle is filled with this effect word before the text is drawn |

- Speeds are pixels per second at 40 frames/s, as in the markup: below 40 it moves 1 pixel every 40/speed
  frames.
- Bit 15 of a move's argument means "only when the text does not fit". The move is skipped when the text fits,
  so one message can be centred when short and scroll when long.

## Example: what Mikohn CON2 0xF7 builds (`mkhn_cmd_message`)

The text is up to 16 characters, and record 1 is used:
```
0x0200FFFF  auto font
0x00010004  show (no motion)
0x01000F0E  centred when it fits; from the right, vertically centred, when it scrolls
0x03000000  hold 0
0x00028014  if too wide: move at 20 px/s
0x06000000  background effect 0 (black)
"TEXT..."   each character with the attract-line FG colour, BG 0
0x00010004, 0x00028014, 0x01000D0D, 0x03000000, 0x06000000
```
