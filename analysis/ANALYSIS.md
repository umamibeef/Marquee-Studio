# Firmware analysis: Mikohn/Neurizon CPU-002 sign

Source: `dumps/bootflash.bin` (512 KB boot flash, AM29LV040B), verified read.

## Deliverables in this folder
- `ghidra_out/elite_rgb_full.c`: complete decompilation of ELITE_RGB: 1916 functions, ~124k lines, zero failures
  (after force-covering code reached only via jump tables). `elite_rgb.c` is the earlier 1522-function pass.
  Code coverage: 617 KB of the 0x4410..0xb2edc code span in functions; only ~8 KB of small gaps remain (pre-entry
  tables/padding). The rest of the image (from ~0x988c4) is the data/text section, decoded separately below.
- `ghidra_out/bootstrap_full.c`: complete bootstrap: 74 functions, ~2.3k lines (vs 14 in `bootstrap.c`).
- `ghidra_out/comms.c`: focused, labelled DB9 download protocol.
- `ghidra_out/string_xref.txt`: every string mapped to the functions that reference it, so grepping for a feature finds its function.
- `ghidra_out/data_map.txt`: the entire data/text section classified line-by-line (1702 strings, 79 pointer tables,
  ~29k other words).
- `ghidra_out/data_section.md`: the data section decoded and categorised (device nodes, file names, config-param
  templates, protocol names, menu/UI, status/comms, RTEMS/libc), plus the notable numeric tables.
- `ghidra_out/*.functions.txt`: address/name/size of every function.
- `app.asm`, `bootstrap.asm`: raw Capstone disassembly.
- `extracted/`: the 33 unpacked archive files (see `tools/extract_bootflash.py`).

## Data/text section (from ~0x988c4 to end of image)
See `data_section.md` and `data_map.txt`. This part of the image holds data only:
- ~1700 C strings: device nodes (`/dev/rs232|rs485|ledpanel|ir|digi`, plus `scc1/3/4_unused`, `smc1/2_unused`
  across two hardware variants), file names (logo/effect .flc, fonts .png, .wav), all UI/menu text, error and status
  messages, and the config-parameter name templates.
- Config parameters are stored as text key=value lines (NVRAM/LFS), keyed by templates like
  `Jupiters,Level,%d,Address`, `Cougar,Level,%d,Jackpot Level`, `WMS,Address`, grouped under 11 tables
  (GENERAL/SMODEM/MODEL/PROTOCOL/JACKPOT/TEXT_LINE/RNG/AUDIO/SECURITY/ATTRACT/GLOBAL). Loader: FUN_0002c4a4 ->
  FUN_00023ae8. Customers/protocols seen: Jupiters, Translux, TGSNCP, Cougar, Mikohn CON2, Casino Link, CCOM, WMS.
- 79 pointer tables: vtables, jump tables, menu and protocol dispatch (incl. 0xb4e48 RS-485 protocol table).
- Numeric tables: 0x988c4 bit-mask lookup; 0xb328c CRC-16/XMODEM (DB9); 0xb3a8c CRC-16/CCITT-reflected (RS-485).

## Regenerating
This needs JDK 21 (JDK 27 crashes JPype/the JVM) and the Ghidra decompiler built for arm64.
- `build/ghidra_12.2_PUBLIC` is Ghidra. The arm64 decompiler was built from source and installed at
  `.../Ghidra/Features/Decompiler/os/mac_arm_64/decompile` (the release ships only x86_64/linux).
- `JAVA_HOME` must point at the JDK 21 install (for example Homebrew's `openjdk@21`).
- `analyzeHeadless ghidra_proj sign -import extracted/ELITE_RGB -processor PowerPC:BE:32:default -loader BinaryLoader -loader-baseAddr 0x0 -scriptPath gscripts -postScript ExportDecomp.java <out>`
- `gscripts/ExportDecomp.java` decompiles all functions; `gscripts/Annotate.java` labels the comms routines and builds the string xref.

## System overview
- CPU/OS: MPC862 (PowerPC, big-endian), RTEMS (paths like `rtems-distribution/cpukit/...`, IMFS in-memory FS).
- Display: `/dev/ledpanel`. The logo .flc animations are 80x16, so the content was made for that size; the
  physical panel size is not confirmed (it comes from the config EEPROM word, not yet read).
- Devices: `/dev/rs232` (DB9 download), `/dev/rs485` (data path, on SCC3/SCC4), `/dev/ledpanel`, `/dev/ir`,
  `/dev/digi`, `/dev/scc1_unused`.
- Ethernet: not used by the firmware. No TCP/IP, socket, DHCP, TFTP, telnet or IP strings anywhere; SCC1 is
  named "unused". The LXT972A PHY brings the link up on its own, which is why the port is electrically alive but silent.

## DB9 download protocol (`/dev/rs232`, 115200 8N1)
Receive state machine `comms_rx_byte_statemachine` (0x33468), dispatch `comms_frame_handler` (0x33A08),
transmit `comms_tx_frame` (0x331A4). CRC-16/XMODEM, table at 0xB328C.
- Host->sign frames start with 0x5A; sign->host with 0xA5: `<sync> <type> <b1> <b2> <hdrCRC16>` then,
  for data types, `<data> <dataCRC16>`. hdrCRC covers sync+type+b1+b2 (parser seeds 0xFBBF = CRC of 0x5A).
- It is only an .lfs file-download server. There is no read, list or dump command:
  - 0x20 start: data = `len(13) "X<8 hex>.lfs" <u32 size> <u16 blocksize>`; with a wrong name or size, the sign replies
    `A5 20` with the name it expects.
  - 0x21 block: `<u16 block#> <data>`; last block also carries `<u32 file CRC>`; replies 0x80 ack / 0x81 nak.
  - 0x10 finish/verify ("Bad CRC"/"Bad File CRC"); 0x90 bumps a counter; 0xBB stops the server ("Server Stop").
  - Idle: every 20 s the sign sends `A5 BB <status>` (0 ok). This heartbeat was the first traffic seen from the sign.
- `tools/signproto.py` builds these frames and models the parser. `whoami` is a read-only probe.

## RS-485 data path (`/dev/rs485`, SCC3/SCC4)
The sign is a slave on an RS-485 bus running a "Jupiters" jackpot/sign protocol (Conrad Jupiters casino, AU).
Configurable per level (menu strings, handler `FUN_000170bc`): Address, Text Rate, Flash On/Off, Graphic Sound,
Broadcast Response, Comms Message. A configurable "Protocol" setting selects the variant. This is how the sign
normally receives its text and jackpot values; the DB9 only downloads files. SCC3/SCC4 stats/diagnostics: `FUN_0000e140`,
`FUN_00012668` ("RS485: Pass/Fail"). The frame format is in `PROTOCOL-RS485.md`. `FUN_00054404`, first thought to
open `/dev/rs485`, is the serial driver's init (see "Serial driver" below).

## Config blocks (NVRAM/flash)
Named parameter groups: GENERAL_CONFIG_PARAMS, SMODEM_CONFIG_PARAMS, PROTOCOL_CONFIG_PARAMS, MODEL_CONFIG_PARAMS,
JACKPOT_CONFIG_PARAMS, TEXT_LINE_CONFIG_PARAMS, RNG_CONFIG_PARAMS, AUDIO_CONFIG_PARAMS. On-screen menu with
User/Setup passwords, Diagnostics, Serial No, View LFS, download settings (direct vs modem), flash timing.

## The 32 MB main flash
Holds the LFS filesystem (downloaded content: fonts, animations, text DB). The boot flash loader checks/erases
"FLASH A"/"FLASH B"/"OB-FLASH" and burns downloaded .lfs files there. Not needed to understand the protocols;
reading it would need BDM (debug port) or desoldering.

## Boot display: the jackpot value animation and the "C1" idle state
Value animation (the number with an effect on the font): the firmware has a full "odometer" value system.
Config params (menu `FUN_000170bc`): Odometer Effect, Odometer Jump (increment step), Odometer Speed (roll rate),
Odometer Background Animation, Odometers Shown, Win Value Effect, Odom(eter) Font, PreWin Frame Count.
The value is drawn in a dedicated Odometer Font over a background FLC (`valueWindow.flc`; `RNGdraw.flc` for a
draw) by the value-window setup `FUN_0000d60c` and the odometer animator `FUN_0000a8c4`, and drawn with the item's Effect FG / Effect BG layers.
The "animation on the font" is the odometer digit roll/count-up (Jump/Speed), plus the Odometer Effect FG.
That is most likely an `effectNN.flc` animation used as the fill inside the digits (see `docs/EFFECTS-AND-LAYERS.md`).
An earlier note credited this to `FUN_0000ef48`. That function is the DIAGNOSTICS test-pattern browser.

"C1" idle state: the scrolled content is data (comms messages or NVRAM config), not a hard-coded string.
The main post-boot display loop is `FUN_0000683c` (loads `logo_e.flc`), driving a display state machine
(`_DAT_0045bfcc` states 1..9; states 4/9 special) with a 4-char shift register that detects keywords like "NONE".
The selected comms protocol's parser (`FUN_00037cf0`) recognises single-letter message types `'6' '8' 'A' 'C'`
(0x36/0x38/0x41/0x43), so `C` is a protocol message/status type. On boot the sign shows the last-known value
(~$2000 from NVRAM); with no jackpot controller on RS-485 it times out (Comms Idle / Link Idle) and scrolls a short
code. Leading interpretations of "C1", both needing the unit to confirm (values live in NVRAM, not the image):
(1) the configured Display ID / address (Translux "Display ID", or the per-level Address); signs scroll their
ID when idle so operators can identify them; "C1" is a typical cabinet code; (2) a comms status code from the
selected protocol ('C' type). Confirm via the on-screen menu (Protocol, Address, Display ID, Comms Message/Idle) or
by capturing the RS-485 line / panel path. Either way it indicates an idle sign with no controller, not a firmware fault.

## FUN_0000ef48 is the DIAGNOSTICS test-pattern browser
An earlier version of this section called `FUN_0000ef48` a text compositor with 52 colour effects. That was wrong.
`FUN_0000ef48` is the DIAGNOSTICS menu handler: a key-driven browser of 52 full-panel test patterns (`]` / `[` to
step, `-` to exit). The real text layers are Effect FG / Effect BG: solid colours, or the `effectNN.flc` slots
(code `0xC000|n`). See `docs/EFFECTS-AND-LAYERS.md`.

## LED panel output path
The CPU draws every pixel in software. Getting pixels onto the LEDs is the job of external
panel logic, fed by DMA. The CPU does no LED scanning.

1. Rendering (CPU). Text, fonts, FLC frames, fills and lines (`FUN_00045860`, `FUN_00045400`) are drawn
   into a RAM frame of W x H RGB555 words. `FUN_00045104` gets a free buffer from the driver, and
   `FUN_00045174` submits it.
2. Driver `/dev/ledpanel`.
   - RTEMS driver table at 0xB2EFC: init `FUN_00059e88`, read `FUN_0005a1e8`, write `FUN_0005a4e8`,
     control `FUN_0005a708`.
   - Init allocates a ring of 8 frame buffers of 0xF000 bytes. That is 320 x 96 x 2, the largest supported panel.
3. Submitting a frame (`FUN_0005a4e8`):
   - The frame is copied into the next ring slot.
   - On some panel types, an optional repack (`FUN_0005a308`) packs the red/green channels of an 80-column,
     48-row panel into 0x1E00 bytes. This looks like a two-colour model.
   - Bit 15 of the pixel word 4 words before the end is set as a frame marker; RGB555 leaves that bit free.
   - The data cache is flushed (`dcbf`) over the buffer, and output starts if it is idle.
4. Output (`FUN_00059408`, set up by `FUN_000590ec`):
   - The CPM's IDMA engine sends the buffer to bus address 0xF0000000. A 16-byte IDMA descriptor holds
     the length, the source (the frame) and the destination.
   - The transfer is paced by DREQ on Port C, so the panel interface asks for data at its own rate.
   - No chip select maps 0xF0000000: the bootloader never programs one, and the app only touches BR1.
   - This points to fly-by / single-address DMA into a FIFO on the panel interface board, latched on DMA
     acknowledge. The exact DCMR mode bits are not decoded yet.
5. Conclusion. The panel side (a CPLD/FPGA or similar) receives a raw RGB555 pixel stream, sees the frame
   marker, and does its own row scanning and 5-bit-per-channel PWM. The CPU sets this up and feeds frames;
   the panel logic takes care of the rest.
   - Brightness and gamma on the real panel are therefore decided by that logic. The studio's brightness,
     gamma and persistence controls are display models, not firmware behaviour.

Board-variant control lines (from config EEPROM bytes 4/5):
- Panel lines: on most boards the app drives PD9 and PD3 for the panel. On "Port C/B" boards
  (`FUN_00055acc`) it uses PC (0x10) and PB (0x80) instead.
- Ethernet conflict: PD3 and PD9 are FEC MII pins. Only Port C/B boards can run Ethernet while the app
  runs. Any board can use it during the bootloader's network window, because the window gives Port D back.

Correction: an earlier note said the 6-byte messages handled by the SCC4 ISR (`FUN_0004f0bc`) came from
panel modules. Nothing ties them to the panel, so that claim is withdrawn. That SCC/TSA link (`FUN_0004e844`)
serves another peripheral, still to be identified; the boot log enables "Sound & Relays" right after it.

## JP2 SW1/SW2 and the board I/O
- Menu input is the infrared remote driver `/dev/ir` (`FUN_00059ad0`/`FUN_00059b54`/`FUN_00059bc4`).
  The menu keys are not jumpers.
- Relay outputs: `FUN_00058da0(n, on)` drives relay 0 on PB22 (PC9 on Port C/B boards) and relay 1 on a
  PCMCIA output pin (PGCR bit 0x40). Global inversion comes from "Relay Polarities". The only I/O screen,
  `FUN_00012c9c`, tests relays A/B.
- Switch inputs: `FUN_00058f74` sets four pins up as inputs: PD8 (PB16 on Port C/B boards), PC14, PA10, PA11.
  Read helpers `FUN_00058e74`, `FUN_00058ecc`, `FUN_00058f04` and `FUN_00058f3c` each have an active-low
  wrapper. None of those four helpers is called in V4.20.
- Correction: one of these pins is used, through a separate accessor,
  `production_jumper_read` (0x12C40). It reads PA10 (PB16 on Port C/B boards), and low means closed.
  - The diagnostics sequence (`diag_sequence_run`, 0x113D0) uses it to choose "Production" diagnostics
    (18 steps) or "LED Display" diagnostics (3 steps). `diag_mode_title_screen` shows which mode is active.
  - `diag_sequence_run` runs at boot when a config flag (`0xDCDDC`) is set, or when a controller sends protocol
    command 3 (`request_diagnostics`).
  - Shorting a jumper at runtime therefore changes nothing until diagnostics run, which matches what was seen on the unit.
- JP2 SW1/SW2 hypothesis: two of these inputs, one of them probably the production-test jumper
  (PA10/PB16). Not proven:
  - `inputs.txt` in the NET=1 bootloader shows which pin each jumper drives.
  - Or close a jumper and trigger diagnostics. With the production jumper the title reads
    "Production Diagnostic" instead of "LED Display Diagnostic".
- FPGA:
  - `FUN_0005a764` configures the Xilinx Spartan at boot: a 300 ms PROG pulse on PA4, status on PIPR bit 25,
    then a bitstream clocked out over SPI.
  - One of four bitstreams (0xA216C, 0xA56B0, 0xA8BF4, 0xAC138) is chosen by board variant and by `FUN_00055f14`.
  - The FPGA is the IDMA target at 0xF0000000 that drives the LEDs.

## Named decompile
- Read this first: `ghidra_out/elite_rgb_named.c`. It is the full decompile, with the project's names, struct types and
  a one-line comment on every function that is understood. `elite_rgb_full.c` is the raw version.
- Single source of truth: `symbols.tsv`, with one line per function, global, struct or RAM region:
  address, name and confidence. `known` means the code shows it; `probable` means the name ends in `_maybe`.
- Typed signatures: a 6th column on a `func` line sets the parameter types (`void;dst:image*,r:rect*,colour:u16`),
  so struct fields appear by name in the body.
- To update: edit `symbols.tsv`, then run `./apply_symbols.sh`. It applies the file to the Ghidra project
  with `gscripts/ApplySymbols.java` and re-exports with `ExportDecomp.java`, in about 2 minutes.
- Coverage after the first naming pass (1,918 functions; later sections supersede these counts):
  - 209 named. About 110 are the sign's own code (by hand), 86 are libpng 1.2.5 / zlib 1.1.4
    (`tools/libmatch.py`), and about 13 are libpng API calls identified by behaviour.
  - 438 tagged by provenance (`prefix` lines in `symbols.tsv`): `os_<addr>` for 260 RTEMS/BSP functions
    (0x61000-0x70000) and `libc_<addr>` for 178 newlib/libgcc functions (0x8C000 onward). These are not real
    names, just "generic code, skip".
  - About 1,270 still `FUN_`: almost all the sign's own code (0x4000-0x61000), plus an unexplained block at
    0x70000-0x7A000. That block holds two huge functions (14.7 KB and 12 KB) next to libpng; it could be more
    libpng or the sign's sound decoder. It is left unlabelled until identified.
- Library matching rules (`tools/libmatch.py`):
  - The tool parses the real sources with clang into an AST (`libsrc/`; stub libc for libpng/zlib, newlib's own headers).
  - A string counts as evidence only if exactly one library function and exactly one firmware function
    use it. For example, "0123456789ABCDEF" is in newlib's `_vfprintf_r` but also in five of the sign's
    formatters, so it proves nothing.
  - Link order fills a gap only on an exact count.
  - Call alignment is limited to each library's address window, which caught an inlining mismatch.
  - Only hand-made names are anchors, and the generated section is rebuilt in full on every run.
- Why newlib and RTEMS are tagged, not named: they have almost no strings, and their one-function-per-file
  archives keep only the members actually used, so exact-count link order never fits. Naming them would need
  fuzzy matching, which means guessing.
- Source of truth: `apply_symbols.sh` resets any user-defined Ghidra name that is no longer in `symbols.tsv`, so every
  name in `elite_rgb_named.c` traces back to a line with its evidence.
- Layout:
  - low board-support code 0x4000-~0x6600;
  - the sign's code to ~0x61000, with its own newlib syscall glue at 0xC874-0xCE7C;
  - RTEMS/BSP 0x61000-0x70000;
  - unknown 0x70000-0x7A000;
  - libpng 0x7A494-0x88568;
  - zlib 0x88738-0x8A90C;
  - newlib/libgcc 0x8C000 onward.

## Runtime structure: RTEMS tasks and semaphores
The main firmware runs on RTEMS (OAR's real-time OS: copyright string, `rtems-distribution/cpukit` paths,
classic API). The bootloader is bare metal. Tasks and semaphores carry four-letter names:
- Tasks:
  - `LEDP`: `main_task` (boot sequence, then the display);
  - `REST`: restart;
  - `CLRF`: storage/buffer check;
  - `SMOD`: download / modem;
  - `DAUD`: audio;
  - one task per wire protocol (`JWAM` `CCOM` `TLUX` `MKHN` `MKCL` `TGSL` `TRBO` `COUG` `BDIU` `WSLL`
    `XPCL` `DPRO`), of which only the configured one starts.
- Semaphores:
  - display/system: `HALT`, `SRST`, `LPSM` (LED panel), `SPIM` (SPI), `IR  `, `PNGS` (PNG decoder);
  - storage and data: `CFSE`, `MODM`, `JPDB` (jackpot database), `TXDB` (text database), `RNGS` (/dev/random);
  - protocols: `CGWS` (Cougar), `JWWS` (Jupiters);
  - audio: `DRMX`/`DREG`/`DREC`/`DPLY` and `MIXR`;
  - RTEMS's own `LBIO`.
- Identified by this: the SCC4 + TSA serial link (`digi_codec_link_init`) is the audio codec link, not the
  panel. The panel is fed by IDMA into the FPGA.

## Display state machine and menu access (display_main_loop)
States (`display_state`, 0x45BFCC):

| State | Meaning |
|---|---|
| 1 | normal: jackpot values |
| 2 | attract/idle, after `cfg_attract_timeout` with no value changes |
| 3 | winner sequence; substates 1..0xB are the steps |
| 4 | on-screen menu |
| 5 / 8 | waiting for data |
| 6 | controller text message (from the text-line database), until new jackpot data |
| 9 | screen opened by a remote key |
| 10 | special message |

Menu access from the IR remote:
- Every key is shifted into a 4-character buffer (`ir_pin_buffer`), which is reset to `"NONE"`. Pressing `!`
  (most likely Enter) compares the buffer with, in order:
  1. a hard-coded 4-character code [redacted]: access level 3, presumably factory/master;
  2. `cfg_menu_pin_level2` (4 digits; -1 = no PIN): level 2;
  3. `cfg_menu_pin_level1` (4 digits; -1 = no PIN): level 1.
- On a match the display saves its state and opens the menu (state 4) at that level.
- Remote buttons for the hard-coded code: [redacted].
- Other keys: `\r` replays the odometer animation in states 1/2/5/6/7; `:` calls `FUN_0000a64c`.

"C1" idle text: most likely an attract message stored in NVRAM.
- Ruled out: state 6 (it needs a controller message) and the ID text table. `ID_TEXT_TABLE` /
  `ALT_TEXT_TABLE` turned out to be the 64-entry machine-number lookup used by `comms_message_dispatch`.
- The attract state (2) cycles through up to 16 lines of `MESSAGE_TABLE`, 17 x 256-byte text lines in
  NVRAM. Each line has its own enable (`cfg_attract_line_mode`), Effect FG/BG word
  (`cfg_attract_line_effect`) and window animation.
- With no lines it plays the `attractNN` animations instead.
- A line containing "C1" (set on site through the menu, or by a controller) would scroll exactly as observed.
- To confirm: fetch `nvram.bin` with the network bootloader and run
  `tools/nvram_decode.py nvram.bin`, which lists the MESSAGE_TABLE lines.

## NVRAM layout (nvram_block_get, 0x5EFEC)
Chain: the battery-backed RAM holds a chain of named blocks from the NVRAM base. The top 0x410 bytes are
excluded; the boot-attempt counter is at 0x7FF0.

Block layout:
- `u8 H`: header length = 1 + name + padding + 8;
- the name, NUL-padded to a multiple of 4. A name of length 4n gets no padding, so its NUL is overwritten by
  the size field;
- `u32 size`;
- `u32 CRC-32` (zlib) of the first H-4 header bytes. The data is not covered;
- `size` bytes of data, with the next block straight after. H == 0 ends the chain.

Behaviour:
- Lookup: a block matches on name and size. A missing block is appended, initialised from the caller's
  RAM default.
- Bad header CRC: the firmware wipes everything from that block to the end and uses RAM defaults.
- Same name, different size (e.g. after a firmware update grows a table): the firmware writes a new header
  into the old block, starting at its size field. The old header's CRC then fails, so at the next boot the
  walk wipes that block and every block after it. In effect, an update that resizes a table resets all
  settings stored after it.

Known blocks: `VERSION_STRING`, `MESSAGE_TABLE` (17 x 256), `TEXT_LINE_DATABASE`, `JACKPOT_DATABASE`,
`ID_TEXT_TABLE` (64 x {u32 id, char[16]}), `ALT_TEXT_TABLE` (64 x char[16]), and the `*_CONFIG_PARAMS` groups.

Tools:
- `tools/nvram_decode.py` decodes a dump (`nvram.bin` from the network bootloader).
- `tools/test_nvram_decode.py` checks it against the firmware's own code under Unicorn:
  - blocks the firmware writes read back exactly;
  - the firmware finds blocks written by the encoder without changing anything;
  - the size-change wipe above is reproduced.

## On-screen menu records (menu_handler)

The full write-up and the generated tree are in docs/MENU.md (`tools/menu_dump.py`). In short:
- `menu_items[192]` at 0xDE42C, 0x58 bytes each (`struct menu_item` in symbols.tsv), filled by `menu_handler(1, 0xFFFF)`.
  Submenus are `menu_list` slices (61 static lists in .data, `menu_list_*`).
- Each field is either a value or a getter, with one flag bit per field (`menu_item_*` accessors at 0x16748..0x16E18).
  That is how items hide (`view_level`), lock (`edit_level`, `menu_item_locked`) or choose a submenu at run time
  (`menu_protocol_submenu`, `menu_download_mode_submenu`, ...).
- 12 item types (`menu_types`), each a `menu_type_ops {enter, draw, key, leave, extra}`.
- The `value` pointers name about 80 config variables (`cfg_*`: Display Number, Protocol, Pool and Jackpot settings,
  audio volumes, RNG options, download settings) and the RS232/RS485 error counters.
- Per-level, per-pool and per-textline settings are arrays of records; `on_open` copies record n into slot 0, the
  edit copy, which the items point at.

## Drawing layer

Everything draws into an `image {format, w, h, pixels, palette}`. Format 1 is RGB555, 2 is 8-bit indexed with a
palette, 9 is RGB555 + 3-bit alpha. A panel frame is an image too: `frame_acquire` wraps a free /dev/ledpanel buffer
with `image_init`, and `frame_submit` hands it back. `point {x, y}` and `rect {x, y, w, h}` are plain i32 pairs/quads.

Primitives: `fill_rect`, `xor_rect`, `fill_rect_black_only`, `invert_rect_mask`, `draw_line` /
`draw_line_bresenham`, `image_blit` (copy or palette; format 9 is skipped, which is why colour emoji needed the patch),
`image_scale` (1/1000 fixed point, optional averaging), `image_slide_init` / `image_slide_step` (straight-line slides),
`rect_grow`, `rect_clip`.

Fonts (`struct font`, 0x618 bytes; `font_table[100]` adds a 256-byte name to each):
- `layout` 0 is a digit strip, 1 a 16x8 grid, 2 a 16x16 grid. A cell is img.w/16 wide and img.h, img.h/8 or img.h/16 high.
- `font_build` measures every glyph from the pixels: for each code it stores blank columns on the left and right,
  the inked width, and a `proportional` flag (0 for space, 0xFF and digits). Digits share one metric (`digit_*`)
  so that numbers don't jump about.
- Text drawing:
  - `text_draw_proportional` draws each glyph trimmed to its ink, plus a 1-pixel column in the bg colour after each
    proportional glyph;
  - `text_width` gives the same width without drawing;
  - `font_draw_text` / `font_draw_text_colours` / `font_draw_cells` are monospace;
  - `font_draw_char_effect` fills a glyph with an effect word;
  - `text_centre_in_rect` and `font_pick_largest` (the largest font whose cell fits a box) handle layout.
- `font_load_builtin` adds the embedded menu font (`LMenuFont.png`). The menu icons come from a second embedded PNG.

Effect words (`effect_resolve`; an effect word is FG<<16 | BG, and `glyph_desc_from_effect` turns it into a
`glyph_blit` descriptor):

| code | meaning |
|---|---|
| < 0x8000 | solid RGB555 colour |
| 0x8000 | transparent (leave pixels) |
| 0x8001 | red/green/yellow cycling texture |
| 0x8002 | stripe texture |
| 0xBFFE / 0xBFFF | colour-table cycle, forward / back |
| 0xC000 + n | effect slot n (`effectNN.flc`) |

Generated textures are cached in 16 slots of 0x510 bytes. `glyph_outline_blit` adds an outline around a glyph, with a
colour from `colour_index_to_rgb555` (0 black, 1 red, 2 green, 3 yellow, 4 blue, 5 cyan, 6 magenta, 7 white). Outlines,
and the "Outline Style/Colour" submenus, are on only when `effects_outline_enabled` is set: panel <= 0x1400 pixels,
or more than 40 MB of RAM.

Odometers (`odometer_draw`, by the pool's "Odometer Style"):
- 0 Roll Up: digits slide;
- 1 Flip Horiz.: width scaled by cos;
- 2 Flip Vert.: height scaled by cos;
- 3 Zoom: scaled about the centre.

Each digit carries a roll phase 0..10000. `odometer_draw` turns it into an angle (×180/10000) and looks it up in
`cos_deg_x1000`. `odometers_tick` moves each pool towards its target at the speed `pool_speed_for_lag` picks from the
Waypoint/Speed settings.

Animations: five banks of FLC+WAV records (0x13C bytes each), loaded by name with `%s%02d.%s`:
- `anim_attract` (100 records);
- `anim_winner`, `anim_altwin`, `anim_loser`, `anim_prewin` (4 each).

`*_anim_play(n, with_sound)` plays one frame per call and starts the sound once. altwin falls back to winner when
missing.

Menu screen: `menu_run(list, level, save)` is the menu main loop. Each item is drawn into `menu_page_a`:
- the label (`menu_draw_title`, top 8 rows);
- the value (`menu_draw_value` calls the item type's `draw`);
- the arrows (`menu_draw_arrows`, from `menu_icon_font`).

Scrolling slides `menu_page_a` against `menu_page_b` over 16 frames (`menu_scroll_up/down`). Select and back slide
sideways in width/40 steps (`menu_select`, `menu_back`, parents on `menu_stack`).

Correction: 0xA8C4 was misnamed `odometer_animate`. It is `rng_draw_run`, the RNG draw sequence: RNGdraw.flc,
pick, letter-by-letter reveal (`text_reveal_step_maybe`), "RNG Range Exhausted" leads to `menu_list_rng_reset`.
0x29D14 was misnamed `menu_downloaded_boot_toggle`; it is `menu_run`.

## Message markup and the compile check

- Message markup (docs/MESSAGE-MARKUP.md, verified by `tools/test_message_markup.py` on the real tokenizer):
  - ESC commands set the colours, entry/exit direction, speed (px/s) and hold, the font, and include another line.
  - A segment ends at CR, ESC or NUL. `textline_step` slides each segment in, holds it and slides it out.
  - Structs `textline` (0x7B8 bytes) and `slide`.
- Compile check: `ExportDecomp.java` also writes `ghidra_out/elite_rgb_named.h`, which has every data type, a
  prototype per function and each global with the decompiler's type. `tools/compile_check.py` compiles the whole
  decompile against it (clang, 32-bit big-endian PowerPC) and maps each error to its function.
  - Int/pointer mixing is not counted as an error.
  - Ghidra artefacts (`_DAT_x`, `uRam…`, register stand-ins) are declared automatically and counted.
  - A sentinel at the end of the file makes sure clang got that far.
  - `apply_symbols.sh` runs it after every export and fails if any function that compiled before stops compiling.
    Otherwise it raises the baseline.
  - `tools/callee_mismatch.py` ranks the callees whose prototypes disagree with their call sites. That is the
    worklist for signatures: one fix corrects every caller.
  - One pass took it from 799 to 1693 of 1918 functions (app: 508 to 1069 of 1168). The jump came from the checker's
    own setup (artefacts, partial-field syntax) plus about 90 real signatures.
- Soft-float helpers were identified by running them (`tools/softfloat_id.py`): `__adddf3`, `__muldf3`,
  `__divdf3`, `__floatsidf`, `sqrt`, `__udivdi3`, and others.
- `./check.sh` (repo root) runs every check: the compile check, the firmware-verified tests and the bootloader
  build/host test. `--full` adds the boot, network and patch co-simulations.

## Config groups, CRCs, JWAM stuffing

- Config groups. Twelve named groups (`cfg_group_names`): GENERAL, SMODEM, MODEL, PROTOCOL, JACKPOT, TEXT_LINE,
  RNG, JACKPOT_BACKUP, AUDIO, SECURITY, ATTRACT and GLOBAL.
  - Each has a RAM shadow (`cfg_*_shadow`, typed as structs where the fields are known). `config_params_load`
    copies a shadow into its NVRAM block (`cfg_group_nvram`).
  - `menu_load_values` (0x28090) fills the menu variables from the shadows; `audio_shadow_from_menu` and others
    write them back. The struct field names come from these pairings.
  - Audio volumes are stored XOR 0x1F1F (a left/right pair of 5-bit values). 0x8000 means mute.
- Menu item 1 is the display geometry editor. Item type 6 (`menu_display_config_*`) edits
  `cfg_model.display_config`, the geometry word, and its label getter shows the version.
  `display_config_next/prev` step through the 26 known geometries.
- CRCs, each checked in Unicorn by `tools/test_crcs.py`, which is part of `./check.sh`:
  - `crc16_ccitt_refl` (Cougar/JWAM receive) is the reflected CCITT step and matches `tools/signproto.py`;
  - `crc16_kermit_update` (JWAM replies);
  - `crc16_modbus` (GASS).
  See PROTOCOL-RS485.md.
- JWAM replies 0x8003..0x8006 are byte-stuffed by `jwam_escape`: bytes below 0x11 become `10, b+0x10`.
  Also checked in Unicorn.
- `fmt_putc_maybe` was mis-guessed; it is `sbuf_insert_char`.

## Serial driver, jackpot protocol handlers

- CPM serial driver (RTEMS driver entry 0xB2F14: `serial_init/open/close/read/write/control`):
  - minors 0..3 are SCC1..4 (`scc_uart_init`) and 4..5 are SMC1..2 (`smc_uart_init`), in the table
    `serial_chans[6]` (`struct serial_chan`, rx/tx `cpm_bd` rings);
  - receive errors come from the BD status bits: FR, PR, BR and OV go to `err_frame/parity/break/overrun`;
  - on the default board `/dev/rs485` is SCC2 and `/dev/rs232` is SMC1. On boards with the panel lines off Port D
    (`board_panel_lines_off_portd`), `/dev/rs232` moves to SCC1 (`serial_dev_names_alt`).
  - `0x54404` was misnamed `rs485_open`; it is the driver's init.
  - Note: `menu_load_values` stores 12 into `serial_chans[1].err_break`. The reason is not known.
- `/dev/random` driver at 0xB2F2C. `random_read` scales a 32-bit value into [lo, hi].
- Jackpot protocol handlers:
  - Mikohn CON2: `mkhn_dispatch` on the command byte; 0xF6 values, 0xF7 message, 0xF9/0xFA sub-tables.
  - Casino Link: '%' value text, '&' value list.
  - XPCL: up to 4 levels of 5-digit fields.
  - CCOM: 7-byte level records, with a nibble-wise reflected CCITT CRC (`crc16_ccitt_refl_nibble`, checked in
    `tools/test_crcs.py`).
  - Pieces of the task loops that Ghidra split off are named `proto_*_task_part_*`.
- `tools/neighbours.py` now shows the total caller count. Unnamed callers had hidden real call sites.

## Random numbers, EEPROM fields, CON2 stubs

- `/dev/random` is MT19937, the original 1998 Mersenne Twister (`mt_sgenrand` seeds with 69069, default seed
  4357; `mt_genrand`). `tools/test_mt.py` runs the firmware's code and matches 2000 outputs of the reference for two
  seeds. The RNG draw (`rng_pick_from_ranges_maybe`) uses it through `random_read`.
- Config EEPROM fields:
  - +0x40 display geometry word, +0x44 options word (bits 0 and 1), +0x48 display number;
  - each 64-byte section ends in a sum/xor pair (`eeprom_checksums_update`);
  - bytes 0x14..0x19 (six) are checked for "present" on non-SPD boards. They may be a MAC address, but that is
    not confirmed (on SPD boards +0x15 is the display type).
- Mikohn CON2: only commands 0xF6 (values) and 0xF7 (message) do anything. Every other command and all 0xF9/0xFA
  sub-commands are empty stubs.
- Packed BCD: values in CON2, Turbo and Casino Link frames are packed BCD (`bcd_to_u32_*`, table
  `bcd_byte_value`).

## Display models (geometry table 0x992B8)

The display geometry word, edited by menu item 1 and stored at config EEPROM +0x40, picks one of 26 models. Each
model is built from 40 x 16-pixel modules. `display_model_name` (0xDC18) spells a word as `[A|E] D WW HH [IM] [+]`:
- WW = (bits 6..11 + 1) / 2 modules across;
- HH = bits 0..5 + 1 modules down;
- E = bit 14 (this bit also decides which outline colours the menu offers);
- IM = bit 12;
- + = bit 6 clear (24 pixels more; no table entry uses it);
- bit 15 is set in every table entry.

The list below comes from running the firmware's own `display_model_name` in Unicorn on each table entry:

| type | word | model | pixels | | type | word | model | pixels |
|---|---|---|---|---|---|---|---|---|
| 0 | 0x80C0 | AD0201 (the fallback) | 80 x 16 | | 13 | 0x81C3 | AD0404 | 160 x 64 |
| 1 | 0x80C1 | AD0202 | 80 x 32 | | 14 | 0x8243 | AD0504 | 200 x 64 |
| 2 | 0x80C2 | AD0203 | 80 x 48 | | 15 | 0x8244 | AD0505 | 200 x 80 |
| 3 | 0x90C2 | AD0203IM | 80 x 48 | | 16 | 0xD0C2 | ED0203IM | 80 x 48 |
| 4 | 0x8140 | AD0301 | 120 x 16 | | 17 | 0xC141 | ED0302 | 120 x 32 |
| 5 | 0x8141 | AD0302 | 120 x 32 | | 18 | 0xC142 | ED0303 | 120 x 48 |
| 6 | 0x8142 | AD0303 | 120 x 48 | | 19 | 0xC1C1 | ED0402 | 160 x 32 |
| 7 | 0x81C0 | AD0401 | 160 x 16 | | 20 | 0xC1C2 | ED0403 | 160 x 48 |
| 8 | 0x81C1 | AD0402 | 160 x 32 | | 21 | 0xC1C3 | ED0404 | 160 x 64 |
| 9 | 0x81C2 | AD0403 | 160 x 48 | | 22 | 0xC241 | ED0502 | 200 x 32 |
| 10 | 0x8240 | AD0501 | 200 x 16 | | 23 | 0xC242 | ED0503 | 200 x 48 |
| 11 | 0x8241 | AD0502 | 200 x 32 | | 24 | 0xC243 | ED0504 | 200 x 64 |
| 12 | 0x8242 | AD0503 | 200 x 48 | | 25 | 0xC244 | ED0505 | 200 x 80 |

When the stored word does not fit the panel, `display_config_read` falls back to 0x80C0 (AD0201).

This sign's panel is 200 x 16 (five 40 x 16 LED boards in a row, counted at the sign 2026-11-02), but its config
EEPROM (read with a Saleae, 2026-11-02) holds **0xC240**: E-series (bit 14), 5 x 1, which `display_model_name` would
spell "ED0501". The table has no such entry, but that does not matter: `display_config_read` never consults the
table. It keeps any word with bit 15 set whose width (((bits 6..11) + 1) >> 1) x 40, + 24 if bit 6 is clear) and
height ((bits 0..5) + 1) x 16 fit `panel_width` / `panel_height` (EEPROM +0x13 = 0xB7: 240 x 128). 0xC240 gives
200 x 16, so the sign runs it as is: **200 x 16, E-series** (the E-series FPGA design, a/c, and the E-series outline
colours). The table is only the menu's list of choices. The stock app in the simulator with word 0xC240 draws
200 x 16 frames lit to column 199. The same EEPROM says board type 2, revision 0x30, 100 MHz.

## Theme file, libio, odds and ends

- Theme file format. The theme is INI text.
  - `theme_ini_find_section` locates `[name]`, and `theme_ini_find_key` finds a `KEY=` line before the next `[`.
  - `theme_lookup_group_key` keeps the parsed groups and keys in two tables (0xE90CC groups, 0xED0CC keys).
  - The values are typed by `theme_get_bool` ("TRUE"), `theme_get_enum`, `theme_get_password` and others.
  - Keys use the same names as the menu (`Pool,%d,Label`, ...). `%d` comes from the `config_key_format*` family,
    which defaults to the pool, text line or attract slot being edited.
- libio. `_read_r`/`_write_r` were swapped guesses. `rtems_libio_read` calls `handlers->read_h` (+8) after
  `LIBIO_FLAGS_READ` (2); `rtems_libio_write` calls `write_h` (+0xC) after `LIBIO_FLAGS_WRITE` (4).
- Display models: see "Display models" above. 26 models from 40 x 16 modules. This sign is 200 x 16, E-series:
  EEPROM word 0xC240 ("ED0501", not in the table; see "Display models"). Five LED boards, each 5 x 2 RGB modules of
  8 x 8 (40 x 16), in one row, counted at the sign (2026-11-02). The "ED" on the board label does match the series.

## FPGA loading, board variants, functional verification

- FPGA: see docs/FPGA.md, checked by `tools/test_fpga.py` against a fake FPGA.
  - `fpga_configure(e_series)` loads one of two Xilinx serial designs: a (= c) for E-series displays, b (= d) for
    A-series. The choice follows `eeprom_display_bit14`.
  - The load uses PROG on PA4, INIT on PB17, PB19 low during the download, SPI at 1 MHz, and DONE on PIPR bit
    0x02000000.
  - Only board type 2 (config EEPROM +4) gets its FPGA from the firmware.
- Config EEPROM: one Catalyst 24WC04 (U1, 512 bytes): I2C 0xA0 and 0xA2 are its two 256-byte halves. +4 board type, +5 board revision, +8 CPU clock (Hz), +0x14..0x19 six bytes (non-SPD),
  +0x40 geometry, +0x44 options, +0x48 display number.
- PCMCIA pins as GPIO:
  - PIPR holds the flash-bank strap bits (boot loader and `flash_storage_init`) and FPGA DONE.
  - PGCRA bit 0x40 is an output pin driving a relay (`relay_set`, `io_init`).
- Verification tools (docs/VERIFICATION.md):
  - `tools/fw.py`, a harness to call any firmware function by name;
  - `tools/test_claims.py`, executable claims per named function. It caught `sbuf_set_hex`: the digits argument is
    exact, not a minimum;
  - `tools/difftest.py`, which compiles the decompiled C of a function back to PowerPC and runs it against the
    original. It found `sbuf_to_int`'s digit test is unsigned in the binary but signed in Ghidra's C.
- `apply_symbols.sh` now refuses duplicate names. Two functions were both called `lfs_volume_size`.

## The audio codec is AC'97

The SCC4/TSA link (`digi_codec_link_init`, `digi_codec_isr`) carries an AC-link to an AC'97 audio codec. That
explains the 6-byte messages earlier left unidentified (see the correction above):
- `diag_hardware_screen` shows the AC'97 mixer registers by name (Master Volume, Headphone, Mono, PC Beep, Phone,
  Mic, Line In, CD, Video, AUX, PCM Out, Record Select/Gain, General Purpose, Power Down) and "Codec Ready".
- Register accesses set bit 7 of the command address for a read (`ac97_reg_write_readback` writes `reg & 0xFE`,
  then reads `reg | 0x80`), which is the AC'97 command format.
- Tag and slot headers 0xE0/0x80 match the AC-link frame layout.

## Controller messages, libc/BSP glue

- Controller messages (CON2 0xF7, JWAM; display state 6): see docs/CONTROLLER-MESSAGES.md.
  - Stored in TEXT_LINE_DATABASE as a byte stream: `1C`+word sets the effect for the following chars, `1B`+word is
    a command.
  - Unpacked into 0x11F4-byte records (`text_line_db_unpack`; `test_claims` runs it), laid out by
    `message_slot_layout` and animated by `message_slot_animate`.
  - Commands: moves 1..5 (with a "only if too wide" bit), 0x0100 alignment, 0x0200 font, 0x0300 hold,
    0x0400 flash, 0x0600 background.
- Start-up and libc glue: `run_constructors`, `bss_clear_maybe`; libio close/lseek/fstat/read/write;
  `isatty`; `_fstat_r`.
- Display geometry helpers: `geometry_width_px` and `geometry_height_px`, with claims.
- Attract/idle switching: `attract_request` and `attract_idle_check` (no comms frames for the Attract
  interval -> attract).
- 247 app functions remain unnamed.

## Sound formats, drivers and theme appliers

- WAV playback supports 8/16-bit PCM, A-law, mu-law, IMA ADPCM and Microsoft ADPCM, mono and stereo:
  `wav_next_*` readers, one rewind per format, and decoders checked against the standards (`alaw_to_linear` and
  `ulaw_to_linear` equal Python's audioop for all 256 inputs; the ADPCM tables are the standard ones;
  `wav_sample_readers` claim).
- Drivers: the `/dev/ir` remote (edge interrupt timed with the timebase, read, open, flush), the SCC and SMC
  UART interrupts, `/dev/digi` write (AC'97), the LED panel IDMA interrupt, and the clock driver (init, and control
  with the magic 'ISR ').
- Theme appliers per destination (`theme_apply_level_*`, `theme_apply_pool_record`, ...). The INI lookup falls
  back from section to group to `[GROUP]`. Menu hooks apply Cougar and modem settings on key 0x3A.
- TEXT_LINE_DATABASE layout: a chain of `{u16 size, u8 id, u8 kind, stream}` (docs/CONTROLLER-MESSAGES.md).
- Misnamed and fixed: `download_buffer_append` (was comms_tx_raw) and
  `tlux_frame_dispatch` (was series2_status_display).
- 165 app functions remain unnamed.

## Every application function named

All 1169 functions in the application range (0x4000-0x61000) now have names: 1307 symbols are `known` and 289
`probable` (names ending in `_maybe`). The last ones were low-level code, identified against the vendored RTEMS
4.6.0 source (THIRD-PARTY.md) or from the disassembly:
- Start-up chain: `app_entry` -> `bsp_start` -> `boot_card` (BSP variables, CPM, I2C, heap;
  `rtems_initialize_executive_early`; exception vectors; constructors) -> `boot_card_start_multitasking`
  (`rtems_initialize_executive_late`: system state BEGIN_MULTITASKING, `thread_start_multitasking`). Two old names
  were wrong: 0x61AA0 is `_late`, not shutdown, and 0x6173C is `_early`. `rtems_shutdown_executive` is 0x61ADC.
- Context switching: `cpu_context_switch` (0x4410; 0x4404 is nop padding), `cpu_context_restore` (0x446C),
  `thread_dispatch`, and the kernel globals (`thread_executing`, `thread_heir`, `system_state`, ...). A claim runs
  the firmware's context switch and checks every register.
- BSP hooks: `bsp_pretasking_hook` -> `bsp_libc_init` (malloc, libio, libc); the postdriver hook is empty.
- 30 optional-manager stubs (`optman_not_configured_*`): RTEMS managers this firmware was built without; each
  raises INTERNAL_ERROR_RTEMS_API / RTEMS_NOT_CONFIGURED (c/src/optman, now vendored).
- Alignment exception emulator (0x40E0-0x43E8): the MPC8xx traps misaligned accesses and dcbz on uncached
  memory; `alignment_exception_dispatch` picks one `align_emu_*` fragment per instruction (lha, lhbrx, lhz, lmw,
  lwarx, lwbrx, lwz, sth, sthbrx, stmw, stw, stwbrx, stwcx., dcbz), each redoing it with lswi/stswi, then
  `alignment_exception_return`.
- Also: `image_scale_blit_nearest` (claim: 2x2 -> 4x4), the theme file tokenizer states, menu record stores,
  `jackpot_record_next_pending`, `ledpanel_output_disable`, `cpm_timer_disable`.
- Hardware notes: ELITE_RGB never talks to the DS1306 RTC (the only SPI traffic is the FPGA download, and it only
  asks RTEMS for ticks). The display model of this sign is not known: AD0201 / 80x16 is the firmware's
  fallback and the content's size, not a measurement.

Remaining work at this stage: turn `probable` names into `known` ones with claims, and name the RTEMS/newlib functions outside the app range.

## Library pass, compile-clean source, claims

Every function is named and every C function compiles.
- No `os_`, `libc_` or `FUN_` placeholder is left (before: 219, 141 and 206). Function rows: 1734 known and 179
  probable (before: 1098 and 250).
- Compile check: 1914/1919 (before: 1766). The 5 functions that do not compile are hand-written assembly; 62
  functions are tagged `[asm]` in all. Of the functions not tagged `[asm]`, 1857/1857 compile.
- 95 claims hold (before: 43).
- Full difftest sweep: 761 equivalent (before: 735), 28 asm, 16 fault_differs, 288 untested, 63 nobuild. The 4
  "different" cannot be equivalent:
  - `eeprom_section_checksum` with an invalid section, as before.
  - `cpu_context_initialize` reads the MSR, so it is now tagged `[asm]`.
  - `glyph_blit` and `display_play_animation` return the caller's r18 or r31 uninitialised on an early path.
- `difftest_full.txt` now lists 682 functions beyond the quick list. `tools/rebuild.py` replaces 605 functions
  (before: 576), all verified from the image.

What the library range holds (names from the vendored sources, with claims where the code runs in isolation):
- RTEMS 4.6: the score kernel (thread, threadq, watchdog, heap, objects, chains, TOD), the Classic API
  directives, libcsupport (malloc family, libio, assoc), miniIMFS (`IMFS_*`) and the newlib hooks. The firmware
  uses miniIMFS: memfile.o is linked only through `IMFS_fchmod`. The newlib hooks come from an older 4.6
  snapshot than the vendored 4.6.0: `libc_start_hook` callocs the reent.
- bzip2 1.0.2 ("1.0.2, 30-Dec-2001", 0x6DDF8-0x7A000; the unknown 0x70000 block). Its error handler is the
  app's `empty_hook`. `lfs_chain_end_maybe` calls `BZ2_bzBuffToBuffDecompress` as well as zlib `uncompress`, so
  LFS entries can probably be bzip2-compressed. The other notes in this project list only stored, zlib and NRV2D.
- libpng 1.2.5, zlib 1.1.4 (the zlib tail 0x8C164-0x8C337 sits inside the `libc` prefix range), and
  newlib 1.11.0 (stdio, strtok, strtol, dtoa/mprec, locale), libgcc (fp-bit, 64-bit division) and libm (pow,
  sqrt, scalbn).
- `__ledf2_hardfloat` (0x98848, probable) is a hard-float compare: it stores with `stfd`. It is reached only
  from `pow` when |y·log2 x| is exactly 1024. On the FPU-less MPC862 that would trap.
- The exception vectors at 0x100-0x1F00 are named (`exc_*`). External interrupts take the SIVEC code / 4 +
  0x18. 0x2344 is 0xFF fill, not code.

Corrections to earlier names (each shown by a claim or the source):
- Clocks (claim `mpc8xx_clock_tree`):
  - `osc_hz` is the PIT/RTC clock (SCCR RTSEL/RTDIV): now `pitrtc_clock_hz`.
  - The old `bus_clock_hz` (0x52D60) is SYNCCLK (DFSYNC): now `sync_clock_hz`.
  - The old `cpm_clock_hz` (0x52E1C) is the external bus clock (EBDF): now `bus_clock_hz`.
- CPM timers:
  - `cpm_timer_set_hz` takes a period in ns, as a toggled TOUT: now `cpm_timer_set_period_ns`. The LED panel
    clock is 280 ns.
  - `cpm_timer_set_count` counts TIN pin edges: now `cpm_timer_set_tin_count`.
- NVRAM:
  - `nvram_global_block_get` always writes the data: now `nvram_block_store`.
  - `nvram_corrupt` copies NVRAM before the wipe: now `nvram_snapshot_take`.
  - `nvram_block_find` searches that copy: now `nvram_snapshot_find`. Config migration uses it.
  - `text_line_db_check_crc` empties the database: now `text_line_db_clear`.
- `pool_record_copy(dst, n)` copies entry n only for n < 0 and entry 0 for every n >= 0. The test in the
  original is inverted.
- libpng: `png_get_int_32` / `png_get_uint_32` / `png_get_uint_16` were really `png_set_oFFs` / `png_set_pHYs` /
  `png_set_tIME`, and `png_read_init_2` was `png_read_init`. This build puts each file's small functions at its
  end, so link order misled the first pass.
- RTEMS:
  - `chain_insert_after` disables interrupts, so it is `_Chain_Insert`.
  - `extension_chain_init` and `extensions_run_hook2/3/4` are the API-extension functions
    (`_API_extensions_*`).
- libgcc: `__dfcmp_a..e` are `__eqdf2`, `__nedf2`, `__gtdf2`, `__gedf2` and `__ltdf2`. The `__udivmoddi4` copies
  also take the remainder pointer.

New claims (selection):
- The firmware's own NRV2D decompressor unpacks the real ELITE_RGB from the boot flash byte for byte. The claim
  uses an encoder in test_claims.py, so it does not need the dump.
- The RNG draws map into the union of ranges and keep a sorted history (`rng_drawn_numbers`).
- PIT tick setup, AC'97 master attenuation, NVRAM block store/snapshot.
- The RTEMS heap and watchdog delta chain, the assoc tables.
- inflate (stored/fixed/dynamic, dictionary, sync), whole PNG decodes with transforms, bzip2 round trips.
- strtok, strtol, the 64-bit division routines, fp-bit, setjmp/longjmp.

Toward a rebuilt binary (`tools/rebuild.py`):
- Fixed after it flagged them:
  - `crc16_kermit_update` and `crc16_ccitt_refl_nibble` return a full register, not 16 bits.
  - `sbuf_remove_at` returns an unsigned byte.
  - `font_build` copies an image onto itself, so difftest now compiles with `-fno-strict-aliasing -fwrapv`.
- The 17 still excluded fall into three groups, which a signature change cannot fix:
  - Garbage on a path the firmware never takes with real input: the firmware returns an uninitialised
    register or stack slot. This covers `sbuf_count_char` with no data (returns r10),
    `jackpot_location_update` (r30) and `comms_rx_callback` with `param_3 == 0`.
  - Faults on random pointers: a fault or hang on one side only (`flc_decode_ss2_*`, `blit_mask_fg_only`,
    `sbuf_init_from`, ...).
  - Probably the check itself: the inputs read the code area, which differs between the two images. Examples
    are `sbuf_pop_char`, `text_width` and `text_char_position`, which difftest finds equivalent over 300 trials.

Tools:
- New `tools/argregs.py`: the real register-argument count, from the machine code.
- `compile_check.py`: the `[asm]` accounting and the rewrites in docs/VERIFICATION.md.
- `difftest.py`:
  - The `asm` category and the two new compiler flags.
  - Edge trials: the first trials set every int argument to a full 32-bit value except one, which is 0. These
    are designed to find the "zero count returns a wide value unmasked" class.
  - Division by zero is skipped: a trial in which the firmware's own divw/divwu divides by zero is skipped.
    That is undefined behaviour in C, and clang removed `effect_texture_8001`'s later `h != 0` test.
- `lfs_chain_end_maybe` is `lfs_volume_append_unpacked` (claim over stored, zlib, NRV2D and bzip2 entries).
  `colour_broadcast_e2d6a`, `level_flags_clear` (one word per level) and `display_select` have claims.
- `ApplySymbols.java`: the `fn` callback type.
