# The simulator and its debugger

`simulator/` is the system simulator in C++: the real boot ROM and ELITE_RGB (RTEMS included) on an emulated MPC862
board, with a debugger window (Dear ImGui) and a GDB server, so VS Code, gdb or Ghidra can attach to the emulated
sign. It is the same board model as the Python simulator in `sim/` (`appsim.py`), checked to compute exactly the same
runs, and about 2 to 3 times faster (35 to 70 million instructions per second; the sign's CPU runs about 40).
Nothing here touches real hardware or serial devices.

| File | Contents |
|---|---|
| `simulator/src/board.cpp` | The board: memory map, boot ROM to app, the special instructions, the SIU and CPM interrupt controllers, the PIT, the CPM UARTs, IDMA (the panel), the I2C EEPROMs, the IR receiver, exact instruction counting. |
| `simulator/src/machine.cpp` | The board under control: one emulation thread owns it; run, halt, step, breakpoints, watchpoints, RTEMS tasks, real-time pacing. |
| `simulator/src/app.cpp` | `umami-marquee-sim`: the debugger window (Dear ImGui on SDL3) and the GDB server. |
| `simulator/src/gdbserver.cpp` | The GDB remote serial protocol over TCP (localhost). |
| `simulator/src/headless.cpp` | `umami-marquee-sim-check`: runs without a window and writes a fingerprint of a whole run. |
| `simulator/src/simtest.cpp`, `simtest_emoji.cpp` | `umami-marquee-sim-test`: the verification runs (stock vs from-source, the serial ports, the menu, the boot-flash image, the console, native emoji), ported from the Python scripts in `sim/`. |
| `simulator/src/elf.cpp` | `umami-marquee-sim-elf`: an ELF with the firmware's names (analysis/symbols.tsv) for gdb. |
| `simulator/src/sign_ui.cpp` | The Sign window: the real sign's firmware over the DB9 (install, run from RAM, console, crash reports, debug record, stage 1). |
| `simulator/src/timeline.cpp`, `ui_style.cpp`, `led_player.cpp` | The timeline view, the fonts and style, the LED panel player. |
| `simulator/src/host/` | Host-side pieces (library `hostcore`): the real sign's serial link and debug record (the Sign window), the display models (`--model`), and the references the firmware checks compare against (native emoji, the firmware's fonts, RS-485 records). Not a Studio: that is the web app. |
| `simulator/tests/test_gdb.sh` | The GDB server driven by gdb; also part of `./check.sh --full`. |
| `third_party/imgui/` | Dear ImGui 1.92.9b (docking branch), MIT. |
| `third_party/stb/`, `third_party/nlohmann/` | stb_image, stb_image_write, stb_truetype (MIT or public domain); JSON for Modern C++ 3.11.3 (MIT). zlib is the system's. |

## Build

```sh
brew install cmake unicorn capstone sdl3 webp gdb
cmake -S simulator -B build/simulator -DCMAKE_BUILD_TYPE=Release
cmake --build build/simulator -j 8
```

The inputs are git-ignored: `dumps/bootflash.bin` (your dump), and for the from-source image `analysis/build/fw/`
(`analysis/tools/fwbuild.py`). A built boot flash image comes from `make -C bootloader fwimage PYTHON=../venv/bin/python`.

## The debugger window

```sh
build/simulator/umami-marquee-sim                                            # the stock dump; gdb server on :1234
build/simulator/umami-marquee-sim --flash bootloader/build/bootflash-fw.bin  # our firmware image (emoji, console)
build/simulator/umami-marquee-sim --app analysis/build/fw/ELITE_RGB          # the from-source app in place of the stock one
```

Options: `--run` (start running; by default it halts at the app entry), `--model NAME` (the display model in the
config EEPROM; default `ED0501` (0xC240), this sign's 200 x 16), `--focus WINDOW` (its ### id), `--cougar` (RS-485 set up for Cougar, as the
RS-485 comparison does), `--gdb PORT`, `--no-gdb`, `--no-window` (the simulator and the GDB server only), `--shot
FILE.bmp --shot-after SECONDS` (save the window and quit, for tests and documentation).

- Toolbar: Run (F5), Halt (F6), Step one instruction (F11), Over (F10: runs to the instruction after a call), Reset
  (boots again; breakpoints are kept). Real time paces the sign at its own speed; untick it to run flat out. The bar
  shows the PC and its function, simulated time, instructions, panel frames and the speed.
- LED panel: the latest frame, live, as LED dots.
- Disassembly: around the PC (Follow PC) or any address or symbol, with the firmware's names; click a line to set or
  clear a breakpoint there.
- Registers: changed ones highlighted; edit when halted (Enter writes). `msr` is the simulator's shadow MSR.
- Memory: a hex view at any address or symbol, and a field to write bytes there. IMMR reads come from the register
  model, without the side effects a CPU read would have.
- Breakpoints and watchpoints (write, read or access), by symbol or address.
- RTEMS tasks: id, name, priority, state, and where each one waits.
- DB9 (`/dev/rs232`, SMC1) and RS-485 (`/dev/rs485`, SCC2) terminals: what the sign sends, and a line to send it
  text (`\xNN` escapes; CR added on Enter if ticked) or hex. On our firmware, `MQ?MON` opens the serial console.
- IR remote: the digits, Menu, Up, Next, In, Back, Exit, or a string of keys.
- Events: the device counters (panel frames, interrupts, PIT ticks, serial bytes, keys).
- Timeline (after Tracy's zones view): which RTEMS task runs when, one lane per task from the context switches, and
  the interrupts, PIT ticks, panel frames, serial bytes per port, remote keys and stops as ticks on a time axis in
  simulated seconds. The wheel zooms around the mouse, dragging pans, Follow keeps the newest events in view, Fit
  shows everything recorded; hover for times and durations. The board records these events only while the window
  is open and Recording is on; they change nothing in a run (the fingerprints stay the same).

The look follows Tracy's use of Dear ImGui (`simulator/src/ui_style.cpp`): Fira Sans for the UI, Fira Code for
code, registers, hex and serial text, Font Awesome icons on the toolbar and window tabs, and a tuned dark (or
light) style with the sign's amber as the accent. The fonts are in `third_party/fonts-ui`.

The layout is dockable and kept in `build/sim/umami-marquee-sim.ini`; Default layout puts it back.

## The simulated sign's serial ports as devices

`umami-marquee-sim` (with or without a window) turns the emulated DB9 and RS-485 into pseudo-terminals and links them at
`build/sim/db9` and `build/sim/rs485` (`simulator/src/pty_bridge.cpp`; `--no-pty` to turn it off). Host tools open
them exactly as they open the real sign's USB serial adapter: `tools/serdump.py --port build/sim/db9 ...`, a terminal
program, and the web Studio's server, which lists them first as "Simulator: RS-485" / "Simulator: DB9" while a
simulator runs (`studio/server.py` `/api/serial_ports`). What a client writes goes into the emulated port; what the
firmware sends comes out. Checked: our firmware's console answers through `build/sim/db9`, and a Cougar record sent by
the server's `/api/send_rs485` to `build/sim/rs485` is counted by the firmware (78 bytes, 1 frame accepted, 0 errors).
A write longer than the firmware's receive buffer continues in its next buffer, as on the real UART (it used to be
cut).

## No Studio here

Umami Marquee Studio is a web app only (`frontend/apps/studio` with `studio/server.py`), with its own in-browser preview.
`umami-marquee-sim` is the simulated sign and its live debugger, plus the Sign window for the real sign's firmware. Designs
reach the simulated sign the way they reach the real one: over its serial ports. (The ImGui Studio windows that
used to live here were removed.)

## VS Code

Open the repository and pick a configuration in Run and Debug (`.vscode/launch.json`; needs the C/C++ extension):

| Configuration | Image | Symbols |
|---|---|---|
| Sign sim: from-source ELITE_RGB (C source level) | the stock boot ROM, then `analysis/build/fw/ELITE_RGB` | `build/sim/ELITE_RGB-debug.elf`: C source for the 601 C functions, names for the rest |
| Sign sim: stock dump (symbols only) | `dumps/bootflash.bin` as it is | `build/sim/elite_rgb_syms.elf`: names from `analysis/symbols.tsv` |
| Sign sim: built boot flash (bootflash-fw.bin) | `bootloader/build/bootflash-fw.bin` | the debug ELF (its app is ELITE_RGB-ext: the same code except the extension sites) |
| Sign sim: attach to a running simulator (:1234) | whatever `umami-marquee-sim` booted | symbols only |

The first three build the simulator and their ELF (pre-launch tasks), start `umami-marquee-sim` themselves (the debugger
window opens too), attach and stop at the app entry. Set breakpoints in the C files under `analysis/build/fw/c/` or by
function name, then continue. The Call Stack view lists the RTEMS tasks as threads. Debug Console commands take the
`-exec` prefix: `-exec monitor tasks`, `-exec monitor ir !`, `-exec x/8i $pc`.

## gdb on the command line

```sh
build/simulator/umami-marquee-sim --no-window &                 # or with the window
build/simulator/umami-marquee-sim-elf                           # -> build/sim/elite_rgb_syms.elf
gdb build/sim/elite_rgb_syms.elf -ex "set endian big" -ex "target remote :1234"
```

```
(gdb) break *text_width          # "break text_width" also works but skips the prologue first
(gdb) continue
(gdb) info threads               # the RTEMS tasks
(gdb) x/8i $pc
(gdb) watch *(int *)0x45c4cc     # _Thread_Executing: stops at the next context switch
(gdb) monitor help               # status, reset, tasks, frame (the panel as text), ir, db9, rs485, serial
```

Source level, with the from-source image:

```sh
venv/bin/python analysis/tools/debug_elf.py              # -> build/sim/ELITE_RGB-debug.elf
build/simulator/umami-marquee-sim --app analysis/build/fw/ELITE_RGB
gdb build/sim/ELITE_RGB-debug.elf -ex "set endian big" -ex "target remote :1234" -ex "break i2c_init" -ex continue
```

`debug_elf.py` compiles every C function of the from-source build again with `-g` (same flags, same linker script),
puts the from-source build's own bytes back into every section, and checks that the image is identical to
`analysis/build/fw/ELITE_RGB.elf`. Clang's `-g` schedules a few instructions differently in about 95 of the 601
functions (it lists them), so in those the line and variable info is approximate. The C is the decompiled source in
`analysis/build/fw/c/` (git-ignored, made from your dump). Use the symbols ELF with the stock dump.

Ghidra's debugger connects through gdb ("gdb remote", target `localhost:1234`, architecture `powerpc:MPC8XX`, on the
ELITE_RGB program at image base 0).

## The protocol

- Target: 32-bit big-endian PowerPC, described in `target.xml` (`qXfer:features:read`) as `powerpc:MPC8XX` with the
  `org.gnu.gdb.power.core` registers: r0..r31, pc, msr, cr, lr, ctr, xer (the app is soft-float). gdb needs
  `set endian big` when it has no ELF loaded.
- Packets: `?`, `g`/`G`, `p`/`P`, `m`/`M`, `c`/`C`, `s`/`S`, `vCont` (c, s, t), Ctrl-C, `Z0`/`Z1` breakpoints,
  `Z2`/`Z3`/`Z4` write/read/access watchpoints, `QStartNoAckMode`, `qC`, `qfThreadInfo`, `qThreadExtraInfo`, `Hg`,
  `T`, `qRcmd` (monitor), `D`, `k`.
- Threads: the RTEMS tasks seen at a context switch, thread id = the RTEMS object id; thread 1 before multitasking.
  The running task has the CPU registers; any other shows the context it saved (pc = its saved LR). Step and continue
  act on the CPU, whichever thread is selected.
- One gdb connection at a time; on attach the simulator halts. Detach lets it run on.

## How the simulator runs

One emulation thread owns the Unicorn instance from creation to `uc_close`; the window and the GDB server send it
work through a queue, which it runs between slices. Instructions are counted per translated block, and a slice ends
on exactly the same instruction whatever the block layout: the block that would cross the end runs its first
instructions alone. Breakpoints stop the same way, exactly before their instruction, so a run with breakpoints that
are not hit is the same run. Devices tick and interrupts are taken between slices of 200,000 instructions (10,000 while
remote-control edges are sent) and whenever an interrupt handler returns with another one pending, as the CPU does.

Watchpoints stop after the access; the instructions of that block are counted whole, so the simulated time can run a
few instructions ahead after a watchpoint stop.

## The verification runs (umami-marquee-sim-test)

`umami-marquee-sim-test` runs the simulator checks without Python. Each subcommand takes the arguments and defaults of the
Python script it replaces, prints the same report, and ends with the same verdict line and exit status:

| Subcommand | Was | Checks |
|---|---|---|
| `compare [IMAGE] [INSNS] [STOCK_INSNS]` | `sim/compare.py` | stock vs IMAGE (default: the from-source build): the distinct panel frames in order. `RESULT: IDENTICAL` |
| `compare-rs485 [IMAGE] [INSNS] [STOCK_INSNS] [AT_FRAME]` | `sim/compare_rs485.py` | one RS-485 Cougar text record: counters, `jackpot_location_update` calls, jackpot records, NVRAM. `RESULT: SAME HANDLING` |
| `compare-db9 [IMAGE] [INSNS] [STOCK_INSNS]` | `sim/compare_db9.py` | read-only DB9 probes: every byte sent back. `RESULT: SAME REPLIES` |
| `compare-menu [--edit] [IMAGE] [INSNS] [STOCK_INSNS]` | `sim/compare_menu.py` | the menu walked with the IR remote (`--edit`: a setting saved): display states, settled screens, NVRAM. `RESULT: IDENTICAL` |
| `flashtest FLASH.bin APP [INSNS] [STOCK_INSNS]` | `sim/flashtest.py` | a complete boot-flash image boots; RAM holds APP; panel frames equal the stock dump's. `RESULT: PASS` (`make -C bootloader fwtest`) |
| `find-culprit [INSNS]` | `sim/find_culprit.py` | bisects the rebuild set (`rebuild.py --only`, still Python) with a simulator run as the test; `MODE`, `MIN_FRAMES`, `TARGET` as before |
| `console [FLASH.bin]` | `sim/test_console.py` | the DB9 serial console of our firmware (`firmware/ext/console.c`). `RESULT: PASS` |
| `native-emoji [FLASH.bin]` | `sim/test_native_emoji.py` | native emoji called inside the running firmware, against the reference in `simulator/src/host/native_emoji.cpp`. `RESULT: PASS` (`./check.sh --full`) |

```sh
build/simulator/umami-marquee-sim-test compare                     # stock vs analysis/build/fw/ELITE_RGB, 80 M instructions
build/simulator/umami-marquee-sim-test compare-menu --edit
build/simulator/umami-marquee-sim-test native-emoji                # pictures in build/sim-test/native-emoji-N/
```

Run it inside the repository (or give `--repo DIR` before the subcommand). The tests that call firmware functions use
`Board::call` (arguments in r3..r10, every register restored afterwards) and `Board::on_exec`; neither changes a run
that does not use them, and the `umami-marquee-sim-check` fingerprints stay byte-identical. Each subcommand was run next to
its Python script on the same inputs with the same results: verdicts, frame counts, console transcript, emoji checks
and pictures. The one textual difference: the console report prints the event counters sorted by name.

## Checked against the Python simulator

`umami-marquee-sim-check OUT.json SCENARIO...` runs the set-ups of `sim/flashtest.py` and the `sim/compare_*.py` scripts
and fingerprints the whole run: every panel frame, the serial bytes, NVRAM, all of RAM, DPRAM, registers, the shadow
MSR and SPRs, the counters and the tasks. `sim/simcheck.py --diff` compares it field by field with the same run in the
Python simulator. All seven scenarios are identical:

| Scenario | Python (s) | C++ (s) |
|---|---|---|
| flashtest (our firmware image, 80 M instructions) | 7 | 3 |
| flashtest_stock (110 M) | 7 | 3 |
| db9_stock (750 M) | 18 | 10 |
| db9_fromsrc (600 M) | 15 | 9 |
| menu_stock (680 M, 20 remote keys) | 18 | 10 |
| menu_edit_stock (680 M, a setting saved to NVRAM) | 20 | 10 |
| rs485_stock (750 M, a Cougar record) | 19 | 11 |

The RS-485 scenario's three Python-only fields (hashes of Python object listings) are not written by the C++ checker;
everything they come from (NVRAM, RAM, frames, serial bytes) is identical.
