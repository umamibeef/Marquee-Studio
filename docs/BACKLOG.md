# Backlog: parked for review later

## Emojis in messages (2026-10-31)
Goal: let messages contain emojis (🍒 💰 🎰 ⭐ …) and show them on the sign.

Starting points. These are untested ideas, to be checked when the work starts:
- Font atlases. The fonts are PNG atlases of 16 columns. Text uses only the ASCII band (rows 2..7, 0x20..0x7F).
  - Rows 0..1 (codes 0x00..0x1F), and any rows past 0x7F, may be free cells.
  - Small emoji glyphs could be drawn into those cells, with each emoji mapped to its code when the message is
    sent.
  - To check:
    - Does the firmware's charmap (0x0B4C6C) and glyph lookup accept those codes?
    - Do the protocols pass bytes outside 0x20..0x7E?
- Colour. Font glyphs are 1-bit masks filled with Effect FG, so emojis would be one colour.
  - A full-colour emoji would need a graphic or animation path instead. One option is a small FLC shown inline or
    as an attract/win graphic, if the firmware can show one alongside text.
- Studio side. Support emojis first in Umami Marquee Studio's preview:
  - Render the emoji into the glyph cell size.
  - Show a palette of emojis that fit the font height (16 px panel).
- Fallback with no firmware change: pre-render the whole message, emojis included, into an FLC animation and play
  it as a graphic.

Update 2026-10-31, from the font code (`font_glyph_locate` 0x444C8, `font_draw_text` 0x43424):
- Font layouts: type 0 is a digit strip, type 1 a 16x8 grid (codes 0..127), and type 2 a 16x16 grid
  (all 256 codes).
- There is no printable-ASCII filter: the character code simply selects a cell. Custom emojis therefore need no
  firmware change:
  - draw them into unused cells of a font PNG (0x80..0xFF on a 16x16 font, or 0x01..0x1F);
  - load that font like any other (fonteNN.png);
  - have the studio map each emoji to its code.
- Text transport: Cougar text records are raw bytes, so 0x80..0xFF pass through (framing escapes
  03/04/10). Only 0x00 (end of string) is unusable.
- Limits:
  - Monochrome: glyphs are masks filled with the item's Effect FG (solid colour or an animated effect).
  - Cell size: one cell is (font width / 16) wide, monospaced; wider emojis span 2-3 codes.
  - Full colour: inline colour emoji would need a firmware patch; colour art fits better as window or
    attract animations (FLC).
- To check: how a font's layout type is chosen (probably from the PNG's grid shape), and the cell sizes of
  the fonts on the sign.

Status: superseded by native emoji (docs/NATIVE-EMOJI.md). The colour-font patch (docs/COLOUR-EMOJI.md) is kept
in the bootloader, but the studio no longer makes colour emoji fonts.

## After the flashable from-source build (parked 2026-11-01)
Both items need our own firmware, so they wait until the from-source ELITE_RGB is finished and verified
(docs/REBUILD-PLAN.md, docs/FLASHING.md).

- Native emoji: done, see docs/NATIVE-EMOJI.md. Not yet run on the sign.
- The sign link (Pi inside the marquee, RS-485) and a serial debugger on the DB9: planned in
  docs/SIGN-LINK-PLAN.md.

## Studio: Cougar needs the sign's level map (found 2026-11-01)
- Correction: an earlier note here said the studio's 69-byte Cougar record was wrong. It is right.
  - The 20-byte field parser it was compared with belongs to TGSNCP, since its only caller is `proto_tgsl_task`.
    It was misnamed `coug_*` and is now `tgsl_*`.
  - Cougar's handler is `comms_message_dispatch` (0x2F544): N x 69-byte records at address 1.
- For a Cougar record to show, the low nibble of its flags (the controller's level) must appear in the
  sign's level map, `cfg_level_address` (0xDCDF0, one byte per display level, set in the menu).
  - A record with an unmapped level is parsed and dropped.
  - The studio's default `level=0` only works if the sign maps level 0.
  - The simulator test maps display level 0 to Cougar level 1 and sends level 1. The value and text are then stored
    (`jackpot_location_update`), the same in the stock and the from-source image (`sim/compare_rs485.py`).
- Drawing it on the panel also needs the sign's pool/theme set-up (which level each window shows). The
  simulated sign has blank NVRAM, so it draws nothing. To do: get an NVRAM dump (network boot loader, `nvram.bin`),
  or configure a pool through the simulated menu.
- Studio: add a way to set the level the sign maps (Settings already has a per-level "Level").

## Studio: check the .lfs layout against the firmware (found 2026-10-31)

`studio/marquee_studio/lfs.py` packs entries as `chk, entry_len, size, flags, namelen, name, pad, data`. The
firmware's entry reader `lfs_entry_to_handle` (0x5C9EC, exercised by the `lfs_entry_nth` claim) reads
`entry_len, size, flags, namelen, name`, with the data at `+0xC + ((namelen + 4) & ~3)` and a check word at the end.
- These differ. Either the studio's packer only fits the boot-flash archive it was first written from, or content
  packs for the sign need the firmware's layout.
- Before any download, check what the DB9 download path (`download_buffer_add_entry`, `lfs_volume_append_unpacked`)
  writes into a volume, and add a claim that the firmware reads a studio-made pack.
- Codecs per the firmware: 0x40000000 zlib, 0x20000000 NRV2D, 0x10000000 bzip2 (bzip2 1.0.2 is linked in). The
  studio now unpacks zlib (any level; this used to be broken) and bzip2, and leaves NRV2D packed.

## Studio: Send to sign over the DB9 (tabled 2026-11-02)
Studio's Send to sign sends a live Cougar record for the sign's RS-485 input (19200 baud). Sent to the DB9 adapter
(RS-232, the download server and our console at 115200), the sign ignores it: nothing reaches the panel. Two ways:
- RS-485: a USB RS-485 adapter on the sign's RS-485 connector (which connector and its wiring still to be checked);
  Studio unchanged.
- DB9: a command in our firmware's console that writes a message line into the message table (as the simulated
  sign does), and the DB9 as a Send destination in Studio. Installs over the DB9, no chip swap. Recommended.

## Boot loader in two stages, so the chip stays put (proposed 2026-11-02)
Every boot-loader change means a chip swap. A small frozen stage 0 in the chip (SDRAM, a recovery window, checks
and boots stage 1 from the main flash, falling back to today's full boot loader after failed tries) and stage 1 in
the main flash, installed over the DB9 like the firmware. One last chip flash, then the TL866A is for recovery.

## Install over the DB9: confirmed on the sign (2026-11-03)
`serdump.py --install bootloader/build-dump/bootflash-fw.bin --reset` installed the from-source firmware (UM0.1:
apps, MQF fonts, no menu, animated emoji) in the update slot: the console restarted the sign into the boot loader's
window (no power cycle), the slot was written and verified, and the slot boot through the RAM stage reached
"firmware healthy (1200 frames)" with no crash, twice (`--boots`). The running firmware's console is today's build
(it has `sraw`; its state is at the new build's address). Firmware updates no longer need the programmer.

## Slot boot in place: the crash explained (2026-11-02, to confirm on the sign)
Booting the update slot with the main flash as the boot volume crashed three times on the sign (watchdog and
checkstop resets). The cause, from the code:
- the first code of ours that runs is the extension loader (analysis/tools/fwext.py loader_asm), at boot_card time.
  It walks the boot volume's archive (RAM 0x2C) to find the EXT entry;
- the main flash has no MMU mapping then. The boot loader's final map covers SDRAM, the IMMR and the boot flash
  only; the firmware's DTLB miss handler maps segment 5 (0x5xxxxxxx) from table[0] (0x1F38) and the list at 0x2144,
  and both are zero until main_task's ram_layout_init_maybe fills them, much later;
- so the loader's first read misses, the handler loads an empty entry (TWC 0, RPN 0), the access misses again for
  ever and nothing serves the watchdog. The stock firmware never meets this: the original boot loader never moves
  the boot volume, and stock code reads it only after main_task has mapped the main flash.
The slot now boots through the RAM stage (SDRAM is mapped), which avoids it. To confirm: one boot with
`serdump.py --slot-mode direct --tries 0`, then `--boots`: expected last step "handed off to the app" (0x51f80000),
reset cause sw-wdog, no crash record (our crash catcher is not in place yet when the loader runs). Then
`--slot-mode stage --tries 0`. If direct mode is ever wanted, the boot loader would have to fill table[0] and the
0x2144 list before the hand-off.

## Hardware notes

- Green noise on the panel while a USB serial adapter is on the DB9 and the laptop runs on its charger: a ground loop
  (the laptop's ground, through the charger to earth, tied to the sign's logic ground). It goes away on battery.
  Fix: an isolated USB serial adapter or a USB isolator; a controller inside the marquee shares the sign's ground.

## Firmware: open after the first flash

- GDB stub on the sign (docs/SIGN-LINK-PLAN.md): not built. The simulator would need the trace exception (MSR[SE],
  0xD00) and trap semantics (tw, SRR1 0x00020000) to test it. Meanwhile: console, crash reports, the simulator's GDB.
- Start-up: after our intro the stock app still shows the MIKOHN logo_e.flc and plays logo_s.wav (stock jingle);
  both are content files that can be replaced like logo_s.flc (tools/make_intro.py, mkimage --replace).
- The stock loading progress bars (rows 12-15 after the intro) are drawn by the app.

## Hardware queue (for when the sign is available)

None of these is needed for the decoding or the studio; each answers a question the code cannot.
1. DONE (2026-11-02, tools/saleae_eeprom.py; captures local only): two power-on captures, identical, section
   checksums OK, no writes. Board type 2, revision 0x30, CPU clock 100 MHz (the simulator assumed 50 MHz),
   +0x13 = 0xB7 (driver frame 240 x 128), geometry word 0xC240 = E-series 5 x 1 ("ED0501", NOT in the firmware's
   table; 0x8240 was expected), options 0, display number 0x17, bytes +0x14..0x19 all zero (no MAC). Reads at
   power-on: 0xA0 block at 0.19 s, 0xA2 block at 0.59 s, 0xA0 again at 1.54 s.
   Settled: the firmware uses 0xC240 as 200 x 16 E-series (no table check). Open: the simulator boots but draws
   no frames with this real EEPROM (even with the word patched to 0x8240): something else in it (board
   type 2?) takes a path the simulator does not model yet.
   Was: Saleae I²C capture at power-on on the config EEPROM: U1, a Catalyst CSI 24WC04J (512 bytes, SOIC-8, date 0330),
   next to the CPU. Pinout from the CAT24WC04 data sheet (Catalyst doc. 1022 rev J, SOIC "J" package): 1 A0 (no
   connect on the '04), 2 A1, 3 A2, 4 VSS, 5 SDA, 6 SCL, 7 WP, 8 VCC; probe 5 SDA, 6 SCL, 4 GND. Its slave address
   is 1010 A2 A1 a8 R/W: a8 picks the 256-byte block, so 0xA0 and 0xA2 are this one chip's two halves (the
   firmware's "EEPROMs 0xA0 and 0xA2"), and one capture gets both. SCL is at most 400 kHz.
   It gives the board type and revision (bytes 4/5), which tells whether it is a Port C/B board, so Ethernet can
   run alongside the panel. It also gives the display geometry (+0x40), the CPU clock (+0x08) and the six
   maybe-MAC bytes (+0x14).
   Result: 0xC240, E-series 200 x 16 ("ED0501"), which `display_config_read` accepts (analysis/ANALYSIS.md). The
   board's "MIK341-077-60ED" label does match the series.
1b. DONE (2026-11-02, photos local only): U24 is a 44-pin quad part under a paper label "CPU-002-01-PLD / 00001427 /
   "C82" / Copyright (c) 2003 Neurizon Pty Ltd" (the name is from the Row Processor's silkscreen). A programmed PLD: the board's glue logic
   (address decode, chip selects), not a code store. The board is part number CPU-002.
   Still to do: the coin cell's type and voltage (if it is flat, dump the NVRAM before replacing it).
1c. DONE (2026-11-02): 5 LED PCBs in a row, each 10 RGB modules of 8x8 laid out 5 across x 2 down (40 x 16), so
   200 x 16. The EEPROM (item 1) says ED0501, 0xC240; the simulator and both studios default to it (`umami-marquee-sim
   --model`, Settings > Panel).
1d. DONE: U17 is not a PROM (marked about "1626 / 25042"); the firmware configures U6. Assumed (not traced): the
   firmware loads both FPGAs, image 2 going to the Row Processor's U8 (docs/FPGA.md).
1e. DONE (2026-11-02): the Row Processor RP-02 (ROW NO. = 0) takes the CPU board's "LED Panel Interface" ribbon
   (20 RS-422 pairs from DS34C87 drivers); docs/FPGA.md.
2. Remote menu, look only (no saving). BLOCKED: there is no IR remote. The same facts come from the EEPROM capture
   (item 1) and an NVRAM dump; an IR transmitter built from the firmware's decoder is the fall-back:
   - item 1: model name / version (unknown; AD0201 would mean the EEPROM word is missing or is the fallback);
   - item 21: "App-BuiltIn:" or "App-Download:". BuiltIn means the running app is the ELITE_RGB already
     extracted. Download means a different image from main flash, which would need extracting.
   - item 29, "View LFS": the files on both volumes.

   Do not change item 21 and do not save: the menu would then write NVRAM 0x7FF0 and change which image boots
   next (docs/FIRMWARE-EXTRACTION.md).
3. Restart the studio and click through the "Message lines" tab (so far tested through the API only).
4. First live RS-485 send of a marked-up record (a long one, so it scrolls), with someone present at the sign.
5. DONE (2026-11-02): U6 is a Xilinx Spartan XCS05XL (VQ100, -4C, date 0341); U1 is the 24WC04 config EEPROM.
6. Firmware extraction, if needed (docs/FIRMWARE-EXTRACTION.md): the NET=1 boot loader on a spare
   AM29LV040B (socketed PLCC), then TFTP the main flash, NVRAM, EEPROMs and boot flash. It never writes flash.
   If Ethernet turns out not to work, a serial-dump variant over the DB9 is planned (not written yet).
7. The box inside the marquee (docs/SIGN-LINK-PLAN.md): where the RS-485 and DB9 connectors are inside the case,
   a mounting place for the box (reComputer R1125-10) away from the LED drivers' heat, its power, the hole for the rear RJ45 coupler, and the Wi-Fi range through the enclosure.
