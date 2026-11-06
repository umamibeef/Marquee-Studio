# Mikohn/Neurizon RGB LED sign: reconstruction spec

This is what an authoring/preview tool and a software emulator need, taken from the boot-flash firmware
(`dumps/bootflash.bin`, decompiled into `analysis/ghidra_out/`). The sign is abandoned; this records how its
content and configuration work.

## 1. Display and panel model
- CPU and OS: MPC862 (PowerPC, big-endian), RTEMS. Display device `/dev/ledpanel`.
- The panel is modular. Its size comes from the board config EEPROM (I2C 0xA0, read at boot into 0x3C7B3C),
  not from the panel hardware (`FUN_00055b80`/`FUN_00055be8`):
  - Most boards: width = `((options >> 4) & 0xF) * 20 + 20` (20..320) and height = `(options & 0xF) * 16 + 16`,
    where `options` is the u32 at EEPROM offset 0x10.
  - SPD boards (byte 4 == 1, byte 5 < 0x50): byte 0x14. If bit 7 is set, width = `((b >> 4) & 7) * 40 + 40`
    and height = `(b & 0xF) * 16 + 16`; otherwise the size is 200x128.
  - This unit is taken to be 80x16, inferred from the content and not measured. The NET=1 bootloader's `info.txt` decodes the real EEPROM.
- Panel output: see "LED panel output path" in `analysis/ANALYSIS.md`. The CPU renders, then IDMA streams
  raw RGB555 words to external panel logic.
- Framebuffer: `width*height` pixels, 16 bits each, RGB555: `bit14..10 = R, bit9..5 = G, bit4..0 = B`
  (5 bits per channel; the panel driver uses bit 15 as a frame marker). The red mask 0x7c00 in the DIAGNOSTICS test patterns (`FUN_0000ef48`) confirms the layout. Text layers (Effect FG/BG) are in `docs/EFFECTS-AND-LAYERS.md`.
- Named colour table (menu "Colour"/"Outline Colour", RGB555 constants): red 0x3c00, green 0x1e0,
  yellow 0x3de0, dim red 0x1c00, dim green 0xe0, dim yellow 0x1ce0, and others. Gradients are generated per channel.
  The physical sign is full RGB, but the content is mostly red/green/amber/gold (the casino jackpot palette).

## 2. Content file formats (all stored in the LFS)
- Animations are Autodesk FLIC (`.flc`), the standard format: magic 0xAF12, 8-bit palette, chunk types
  COLOR256(4)/BRUN(15)/SS2(7)/LC(12)/BLACK(13)/COPY(16). Palette RGB(0..255) becomes RGB555 by `>>3` per channel.
  Sizes seen: logos 80×16, effects 16×16 / 80×16 / 2×60, and the jackpot windows (valueWindow, winnerWindow,
  NewJPWindow and others). Any modern FLIC writer can produce them; `emulator/sign_emu.py` decodes and previews them.
- Fonts are PNG glyph atlases (`fonte01..08.png`, 8 sizes from 112×160 to 320×512; also nuFont, LMenuFont, MenuIcons).
  They are 1-bit, white glyphs on black. Layout: 16 columns (cell width = img_w/16), with the ASCII block 0x20 to 0x7F in 6 rows at
  a row pitch of img_h/16, below a large-digit row and above a currency row. The charmap for the built-in menu font
  (`0x0b4c6c`) is `ABC…Zabc…z012…9` plus symbols (87 chars). `emulator/font_render.py` extracts and renders glyphs.
- LFS container (the downloadable content image; the boot archive uses the same layout): a flat archive of
  entries starting at offset `u32@0x04`, each `u32 chk, u32 entry_len (incl. 16-byte header), u32 size, u32 flags,
  u8 namelen, name, pad, data`. Flags: `0x40000000` zlib (size = inflated), `0x20000001`/`0x20000000` UCL NRV2D
  (bootstrap decompressor at 0xC90), `0` stored. `tools/extract_bootflash.py` unpacks it. The per-entry `chk` is a
  non-standard checksum. It is unidentified and not needed to extract into RTEMS IMFS.
- Config and theme files are text: `key=value` lines under `[GROUP]` headers (e.g. `Download,Mode=direct`,
  `Download,Datarate=115200`). Parameters are grouped in 11 tables: GENERAL, SMODEM, MODEL, PROTOCOL, JACKPOT,
  TEXT_LINE, RNG, AUDIO, SECURITY, ATTRACT, GLOBAL (loader `FUN_0002c4a4`→`FUN_00023ae8`). Keys look like
  `Jupiters,Level,%d,Address`, `Cougar,Level,%d,Jackpot Level`, `WMS,Address`. See `analysis/data_section.md`.

## 3. Message and display model
Content is organised by level (jackpot level/line). A jackpot/text broadcast record (69 bytes, §4 of
PROTOCOL-RS485.md) carries `flags` (level nibble, 0x20 show-text, 0x40/0x80), two u32 values (amounts)
and 60 bytes of text. The firmware composes a frame per level from a background FLC window, the value rendered in a
font, and the text, with effects (flash on/off timing, text scroll rate, odometer roll of the value). Menu parameters
per level: Text Rate (scroll speed), Flash On/Off, Graphic Sound, Colour/Outline Colour, and fonts (including Odometer
Font). The `*Window.flc` animations are the frames behind the value, winner, description and new-jackpot messages.

## 4. Getting content on and off the sign
- DB9 `/dev/rs232` (115200 8N1) is a download-only server that writes flash. Frames: host sync `0x5A`,
  sign sync `0xA5`, `<sync><type><b1><b2><hdrCRC16-XMODEM>` plus optional `<data><dataCRC16-XMODEM>`.
  Download sequence: `0x20` start (data = `[13]"X<8 hex>.lfs" <u32 size> <u16 blocksize>`; the 8 hex digits come from a
  RAM word, so if you send a wrong name once the sign replies `A5 20` with the exact name), then `0x21` blocks
  (`<u16 block#> <data>`; the last block also carries `<u32 file CRC-32>`), then `0x10` finish/verify. The file CRC is standard
  CRC-32 (`FUN_00088738`, table 0xb1c50). See `analysis/comms.c` and `tools/signproto.py`.
- RS-485 RJ45 (SCC3/SCC4) is the live content path. It updates the display, not flash, and the sign acts as a jackpot-bus slave.
  The full spec is in `analysis/PROTOCOL-RS485.md`; the builder is in `tools/signproto.py` (`rs485_text_frame`). It needs a
  ~$10 USB-RS485 adapter and the NVRAM config (selected protocol, level ids, baud) read from the menu.
- The firmware does not use Ethernet (no stack; SCC1 is "unused").

## 5. Emulator (`emulator/`)
- `sign_emu.py`: RGB555 `Panel`, `FLIC` decoder, GIF/PNG export. It renders the real MIKOHN boot logo correctly.
- `font_render.py`: `SignFont` loads a `fonte0N.png` atlas and draws proportional text in any RGB555 colour.
- `demo.py`: boot logo, then a scrolling jackpot message on the 80×16 panel, written to `demo_sign.gif`.

Together these are the start of an authoring preview that composes FLC, text, colours and effects and shows them as the sign renders them.

## 6. Roadmap for the authoring and config tool
1. Preview (done): panel, FLC, fonts and scroll/flash rendered live. It still needs the window/value compositor
   (odometer value render, window FLC behind text), which means following the Effect FG/BG blit and the `*Window.flc` users (not `FUN_0000ef48`, which is DIAGNOSTICS).
2. Content packer: write an LFS image (same container format) with custom FLC, fonts and config, then upload it
   with the DB9 download sequence (CRC-32), or push live frames over RS-485.
3. Config editor: edit the `[GROUP] key=value` theme and parameters (the 11 tables) and ship them in the LFS.
