# Rebuilding ELITE_RGB from source (plan of 2026-10-31)

The goal is to finish the decompilation far enough to compile a binary that can be flashed onto the sign and is
functionally identical to the stock ELITE_RGB V4.20.

Status when the plan was written: every app function is named. 1766 of 1919 functions compile (app range: 1128 of 1169). 735 app
functions are proven equivalent to the firmware by `difftest` (recompiled C vs firmware, every byte of RAM compared on
random inputs). The C rebuild of the boot loader already boots the stock app, byte-identical in co-simulation.

## 1. Approach: a same-layout rebuild, one function at a time

A full relink with a new memory layout would change every address at once, and a mistake anywhere would be hard to
find. The rebuild therefore keeps the stock layout:

1. The identity image. `analysis/tools/rebuild.py` builds `ELITE_RGB` from your own extracted image and our
   function list, with every function still the original bytes. It must be byte-identical to the stock image,
   which proves the pipeline.
2. Replace functions one at a time, at their own addresses. Each replaced function is our recompiled C (or
   assembly where it needs it), linked with every callee and global at its original address, exactly as `difftest`
   already does. Calls, globals, tables and data keep their addresses, so a replaced function cannot break the rest.
   - If it fits (76% of the 715 that build at `-Os`), it goes in place, and the rest of its old space becomes a hole.
   - If it is larger, it goes into a hole left by a function that shrank, and its original entry becomes a 4-byte
     branch. In total, recompiled code is 93% of the original size, so the holes are expected to suffice without
     growing the image.
   - Only proven functions are replaced: `difftest` equivalent, or assembly, or covered by a claim. Functions that
     are entered in the middle (jump-table fragments such as `caseD_*` and the theme/Cougar parser states, shared
     tails) are found by scanning every branch and pointer in the image. Such a family is replaced together or not at
     all.
3. Grow the replaced set until every function is ours. The image is then built from source, still at the stock
   layout.
4. Optionally, afterwards: a normal relink, with a fresh layout and the data emitted as C/assembly. This comes only
   once the same-layout build is complete and verified.

## 2. What the image contains, and the copyright line

- Code: 0x0000 to about 0x988C4: the exception vectors, the app, RTEMS, newlib, zlib, libpng.
- Data: to 0xBDDF4: 1702 strings, pointer tables, constants, the four FPGA bitstreams (`fpga_bitstream_a..d`), the
  tables the code uses. Then .bss (zeroed at start-up).
- The data, and any original code bytes still in the image, are Mikohn's.
  - The build reads them from your own extracted image at build time, as the boot loader build already does with
    your dump.
  - Nothing derived from the firmware is committed. The binary guard and the allow-list `.gitignore` enforce this.
  - The repository holds only our code, our tools and the address map.

## 3. Verification ladder

1. Byte identity of the identity image (step 1).
2. Per function:
   - `difftest` equivalence: random inputs, every byte of RAM compared;
   - the claims;
   - for assembly: the disassembly matches what it replaces.
3. Whole image, in co-simulation. The stock image and the rebuilt image run side by side in Unicorn on the
   emulated board, extending the boot loader's co-simulation. The emulator models what the app touches:
   - the decrementer and timer interrupts;
   - the serial channels (SMC1, SCC2), I2C (the config EEPROMs), SPI (the FPGA download);
   - the LED panel IDMA (frames are captured);
   - NVRAM.

   Compared over minutes of emulated time, from the same inputs:
   - every panel frame;
   - every byte sent on the serial ports;
   - every NVRAM write;
   - the state after start-up.

   Scripted inputs:
   - a boot;
   - attract mode;
   - RS-485 records with marked-up text;
   - the menu over DB9 (look only);
   - a jackpot hit.
4. On the sign, without writing flash. The `NET=1` boot loader, on a **spare** AM29LV040B (the boot flash is
   socketed; the original chip is kept), loads an app image over TFTP **into RAM** and runs it. The main flash is
   never touched. Compare with the stock app: the same content, the same RS-485 records, the same panel.
5. **Flashing the rebuilt app into the main flash is a deliberate decision made only at the end, with a dump of the
   main flash taken first** (the serial dump window) so that the stock app can always be restored.

## 4. Work items

| # | Item | Status |
|---|---|---|
| A | Make every function compile (argument counts, types) | in progress (1766/1919) |
| B | `rebuild.py`: identity image, in-place replacement, hole packing, interior-entry scan, verification from the image, report | **done** (first run below) |
| C | Assembly for what C cannot express: `nrv2_getbit`, context switch/restore, the alignment-exception emulator, exception entry/return, `mmu_enable`, the low vectors | tagged first |
| D | `difftest` coverage for the rest: the 304 "untested" (every random trial faulted) need structured inputs (valid structs, handles) | open |
| E | Whole-image co-simulation (section 3, item 3) | **milestone 1 reached** (`sim/appsim.py`): the stock app and the from-source build both boot through the RTEMS start-up to the first context switch |
| F | RTEMS, newlib, zlib, libpng: rebuild from the vendored sources where they match exactly, or recompile the decompiled functions like the app | open |
| G | Packaging: the rebuilt app into the archive the boot loader loads (`bootloader/tools/mkapp.py`) and the network boot | tools exist |
| H | The test on the sign (section 3, item 4) | when the sign is back |

## 4a. First same-layout rebuild (2026-10-31)

`analysis/tools/rebuild.py --trials 24`:
- the identity image is byte-identical;
- 576 functions replaced (502 in place, 74 moved into holes): 12.9% of the code bytes are now our compiled C
  (`-Os`);
- every one verified from the rebuilt image (575 equivalent, 1 untested).

Not replaced (159):

| Reason | Count |
|---|---|
| larger than their slot, no hole left | 76 |
| entered other than at their start | 20 |
| jump through a register | 19 |
| differ from the firmware when run from the image | 23 |
| renamed since the sweep, or other | 21 |

The 23 that differ were "equivalent" to difftest, but the image check's wider random inputs found real precision
bugs in the decompiled C:
- 32-bit values kept in parameters typed 16-bit (`crc16_kermit_update`);
- sign extension of returns (`sbuf_remove_at`);
- off-by-one (`sbuf_count_char`);
- different behaviour on invalid input (division by zero in `effect_texture_8001`).

They are the top-priority fix list, alongside the compile errors.

Next steps: more room for larger functions (a reserved overflow region, as the colour-emoji patch does at the bottom of
the C heap); structured inputs for the "untested"; the whole-image co-simulation.

## 4b. ELITE_RGB linked entirely from source (2026-10-31)

`analysis/tools/fwbuild.py` links ELITE_RGB entirely from source:
- C: 611 functions, compiled at `-Oz` from the decompiled C, in place or moved into holes with a branch at the
  original entry. Every one is verified from the image (`rebuild.py`).
- Assembly: everything else in the code region, from `asmsrc.py`. It disassembles the code with a label for
  every branch target (11,955 of them) and keeps `.long` only for what LLVM cannot reproduce: 2.6% (tables, padding,
  a few encodings). It reassembles the whole code region (156,209 words) byte-identical to the stock image.
- Data: the data region, `.incbin`'d from your extracted image at build time.

Result: byte-identical to the verified `rebuild.py` image. Packaged with `bootloader/tools/mkapp.py`, it boots in
`netsim` through the network boot loader's TFTP upload into RAM, with no flash written. The RAM afterwards holds
exactly this build.

Functional identity at this point:
- the assembly and data parts are the stock bytes;
- each of the 611 C functions is equivalent to the stock function on random inputs, run from the image;
- whole-image behaviour (scheduling, devices, timing) was not yet checked; that is the simulator's job (item E).

C coverage grows as functions are proven, through:
- structured inputs for the ~300 that random inputs cannot exercise;
- the simulator for the rest;
- the 17 that read uninitialised registers in the original, which need assembly or exact emulation of that quirk.

## 4c. System simulator boots both images (2026-10-31)

`sim/appsim.py` runs the real boot ROM from your dump, then the chosen app image, on the emulated MPC862 board
(`bootloader/test/cosim.py`'s board, extended):
- the MPC8xx special instructions are intercepted over the app too, with a shadow MSR (411 sites);
- the SIU interrupt controller and the PIT (RTEMS's clock tick) are modelled;
- all 128 MB of SDRAM is mapped.

Both images get through:

| Point reached | Stock app | From-source build |
|---|---|---|
| `boot_card` | 2.84 M instructions | 2.84 M |
| `rtems_initialize_executive_early` | 63.8 M | 63.8 M |
| the first `cpu_context_switch` | 65.36 M | 65.34 M (fewer instructions: our compiled C) |

Next steps:
- Speed. About 1 M instructions/s at this point, 40 times slower than the sign. The per-slice instruction counting
  costs the most.
- Interrupt delivery in multitasking.
- The devices the tasks touch: UARTs, the IDMA panel output, the SPI FPGA load, I2C from the app's own driver,
  the CPM interrupt controller.

## 4d. Whole-system run matches stock (2026-11-01)

The system simulator now runs both images through start-up into the boot animation:
- the same five tasks: INIT, LEDP, REST, SMOD and the random task;
- the same panel output: 48 of 48 distinct frames identical. Our build is faster, so the stock run gets a
  longer instruction budget to reach the same point (`sim/compare.py IMAGE 80000000 110000000`).

Three functions broke the match. The simulator found them; random-input tests could not:

| Function | Cause | Fix |
|---|---|---|
| `flc_next_frame_header` | Ghidra split the 16-byte FLC chunk header on the stack into separate locals. Recompiled, they were laid out apart. | a `stack` row typed `flc_chunk_hdr` |
| `image_load_png_mem` | The same with libpng's `{base, pos, size}` source: no PNG decoded, so no fonts, so no frames. | a `stack` row typed `png_mem_src` |
| `ac97_link_send` | A bit-banged AC-link. Ghidra treated the watchdog, port and tick-counter memory as constants, dropped the writes as "unreachable" and made the wait endless. | tagged `[asm]` |

Rules added so these classes cannot come back:
- `rebuild.py` never replaces a function tagged `[asm]`, or one whose decompile says blocks were removed as
  unreachable.
- A scan for `&local` passed to a callee next to adjacent locals found 11 more split structs (the font origin, the
  `rtems_io_read/write/control` argument blocks, window and digit rects). They are fixed with `stack` rows.

Flashable image:
- `make -C bootloader fwimage`: the original boot loader and content, ELITE_RGB from source, NRV2D-compressed by
  our compressor (`bootloader/tools/nrv2d_pack.py`). It fits the 512 KB chip with 63 KB free.
- `make -C bootloader fwtest` boots that whole image in the simulator: the boot loader unpacks our app byte for
  byte, and the panel output equals stock. Result: PASS.
- These checks now run in the C++ simulator: `make fwtest` uses `umami-marquee-sim-test flashtest`, and the comparisons
  below are `build/simulator/umami-marquee-sim-test compare` / `compare-rs485` / `compare-db9` / `compare-menu`, with the
  arguments of the Python scripts named here (docs/SIMULATOR-GUI.md).
- How to program a spare chip: `docs/FLASHING.md`.

Longer runs:
- A fourth culprit appeared at 183 M instructions: `image_load_png_file`. Ghidra split the LFS file handle's read
  position off the handle, so libpng re-read the PNG signature, failed a CRC and `abort()` shut RTEMS down. The
  handle is now a `stack` struct (`lfs_file`).
- Since that fix, both images match over 1.5 B (ours) and 2 B (stock) instructions, about 3,250 panel frames:
  - 95 of 95 distinct frames identical;
  - the same tasks;
  - the same `display_state` changes (normal display, then attract) at the same frames.

## 4e. RS-485 reception (2026-11-01)

`sim/appsim.py` now models the CPM UARTs:
- received frames go into the channel's rx buffer descriptors, with the SCC event and the CPM interrupt;
- tx descriptors are completed and their bytes captured;
- SCCE/SMCE are write-1-to-clear.

`sim/compare_rs485.py` forces `cfg_protocol` to Cougar in both images (the simulated NVRAM is blank) and sends the
same frame after the same number of panel frames. Results:
- Both images handle it the same: `/dev/rs485` (SCC2) opened, 78 bytes received, 1 frame dispatched, 0 errors,
  the same payload, nothing sent back.
- Panel: 92 of 95 distinct frames identical. The other 3 are the start-up progress bar caught at a slightly
  different length. With the Cougar task competing for the CPU, our faster code is at another point of the load.
  The bar grows to full in both, and the sequences realign.
- Not covered here: a jackpot shown on the panel (see the update below).

Update (2026-11-01): the record above used level 0 on a sign with no level map, so it was parsed and dropped.
`compare_rs485.py` now maps display level 0 to Cougar level 1 (`cfg_level_address`, 0xDCDF0) and sends a level-1
record. `comms_message_dispatch` then stores its value and text with `jackpot_location_update`. The test compares
each update call, the four jackpot records and the whole NVRAM.

Drawing the value also needs the sign's pool/theme set-up, which blank NVRAM lacks. Cougar records do not use 20-byte
fields, as an earlier note said; that parser is TGSNCP's (now `tgsl_*`).

## 4f. DB9 and the remote-control menu (2026-11-01)

DB9 (`sim/compare_db9.py`): both images get the same read-only probes on `/dev/rs232` (SMC1): `whoami`, an
invalid 0x20 start header, and a header-only 0x90.
- First result: stock answered `X00C1A042.lfs`; ours answered `X00\x001*0\x072.lfs`.
- Cause: `build_expected_lfs_name` copies `"0123456789ABCDEF"` onto the stack and indexes it by nibble. Ghidra
  split the copy into four `char[4]` and a `char`. Recompiled, those were laid out apart, so digits 4..15 came from
  other stack slots.
- Fix: a `char[17]` stack row. `menu_serial_no_text` had the same copy.
- After the fix: the same replies, byte for byte.

Remote control (`sim/appsim.py` `ir_press`, `sim/compare_menu.py`):
- Keys are sent as pulse-distance frames on PC13, with the timing from the thresholds the app computes in
  `ir_init_maybe`. The app's own `ir_edge_isr` decodes them; inside it, `mftb` returns the edge times. With blank
  NVRAM there is no PIN, so Enter (`!`) opens the menu.
- The walk is 20 navigation keys, with no edits.
- First result: all menu text was garbled blue.
- Cause: `font_draw_char` / `font_draw_char_part` pass `glyph_blit` a 16-byte style, `{3 words, fg, bg}`.
  Ghidra split it into `u32[3]` and two words.
- Fix: the `glyph_style` struct.
- A wider scan also found that `mkcl_cmd_values_text` had a 12-byte buffer for the 20-byte jackpot location
  record.

After these fixes, the long run (`sim/compare.py`, 450 M / 600 M) shows 3 of 95 distinct frames different, at
48..50: the start-up progress bar caught at another length. Our sequence hash there, `c5204b6a01114bed`, is exactly
the one the stock image produces when the Cougar task slows it (4e). These are frames stock draws too under a
different load, so the difference is one of timing. RS-485: 95 of 95 identical. Menu: 42 of 42.

Measured with one script, three runs:
- our build: `c5204b6a01114bed`;
- stock with only the CPU load changed (the Cougar task on): the identical 95 frames;
- stock without it: `ee6ae673f6ca5e22`. Against that run, our frames 48..50 differ only in panel rows 12..15,
  the strip where the start-up progress bar is drawn.

Stock itself therefore produces both sequences, depending on load.

Menu edit (`sim/compare_menu.py --edit`): change "Display No." 0 -> 1 and confirm it. 79 of 79 frames are
identical, the display states are the same, and the 32 KB NVRAM is byte-identical afterwards. The config save
path (NVRAM blocks, CRCs) behaves the same.

The scan for split stack objects now also covers:
- `xStack_` names and `word`/`dword` locals;
- arrays indexed with a variable;
- arrays that are word-copied into.

## 4g. Assembly source with every instruction decoded (2026-11-01)

`asmsrc.py` used to keep 2,033 instruction words as `.long`, because LLVM re-encoded them differently. Two causes:
- Conditional branches with the old-style prediction bit (BO "y" bit). Capstone drops it, and LLVM's `+`/`-`
  encode the newer "at" bits. Such branches are now written in the raw form (`bc 13, 2, L_...`,
  `bclr 5, 2, 0`), which encodes every bit as given.
- Condition-register logic printed as aliases (`crnot`, `cror` with field names). It is now written with
  explicit bit numbers (`crnor 2, 2, 2`).

Result:
- 154,212 of 156,209 code-region words are instructions in the source.
- The 1,997 `.long` left are data inside the code region (jump tables, constants: 1,995) and the 2-word text
  header at 0x4.
- The 63 `[asm]` functions now have none.
- The whole region still reassembles byte-identical; `fwbuild.py` is IDENTICAL, and `make fwtest` passes.

## 5. Safety rules

- **Nothing writes the main flash unless the sign's owner decides to, at the end, with a dump of the main flash taken first.**
- Hardware tests use a spare boot-flash chip, and the app is loaded into RAM.
- No firmware-derived bytes in the repository.
