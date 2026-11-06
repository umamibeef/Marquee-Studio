# Serial debugging on the DB9

The DB9 on the sign is `/dev/rs232`: SMC1 at 115200 8N1 on the default board (SCC1 on boards with the LED-panel
lines off Port D). The firmware's download server listens there. Two read-only tools share the port with it:

| | Boot-time dump window | Console |
|---|---|---|
| Where it runs | our boot loader, before the app (`make DUMP=1 fwimage`) | in the app (firmware extension, every `fwimage`) |
| How it opens | the host sends `MQ?DUMP` + CR within about 2 s of power-on | the host sends `MQ?MON` + CR at any time while the app runs |
| What it does | reads out the boot flash, both main flash banks, NVRAM, both EEPROMs and SDRAM, CRC-checked | memory hex dump, a RAM word write, status |
| Host tool | `tools/serdump.py` (framed binary protocol, resumable) | any terminal (`screen`, `picocom`, `tio`) |
| Works if the app crashes | yes | no |

Neither can write flash, NVRAM or an EEPROM, or send a download-server frame.

## Wiring

A USB serial adapter with RS-232 levels (the PL2303 used before) to the sign's DB9, 115200 8N1, no flow control.
Find the port with `ls /dev/cu.*`.

## Boot-time dump window

Needs the image built with `make -C bootloader DUMP=1 fwimage PYTHON=../venv/bin/python`, which writes
`bootloader/build-dump/bootflash-fw.bin`. That image starts with our rebuilt boot loader (BOOTSTRAP V2.00 in C)
instead of the original one. It has passed every check in the simulator, but it has not yet run on the sign, so it
goes on a spare chip (docs/FLASHING.md).

```sh
venv/bin/python tools/serdump.py --port /dev/cu.<adapter>          # start it, then power the sign on
venv/bin/python tools/serdump.py --port /dev/cu.<adapter> --speed 460800   # faster, if the adapter manages it
```

Files go to `dumps/serial/` (`info.txt`, one `.bin` per region, `manifest.txt`). The tool resumes partial files and
checks every region by CRC, then sends BOOT and the sign starts normally. Without the magic the window closes by
itself after about 2 s and the boot goes on as usual. Details: `bootloader/README.md`, "Serial dump window".

Checked in the simulator (`bootloader/test/fwdumpsim.py` on that image): the window answers; the boot flash, NVRAM and
both EEPROMs read over serial equal the board's contents; BOOT, or no magic, boots the from-source app; no store to
flash, NVRAM or CS7, every I2C transaction a read.

## Console

In every from-source image with the extensions (`firmware/ext/console.c`). Open a terminal on the port, type
`MQ?MON` and Enter. The sign answers `MQMON 1 marquee-sign console (help)` and a `mq> ` prompt.

```
md ADDR [LEN]   hex dump (RAM, boot flash, NVRAM)
mw ADDR WORD    write a word to RAM
info            RAM, heap, emoji, console
reset           restart the sign (the firmware's own restart, never erasing NVRAM)
exit            back to the download server
numbers are hex
```

- `md` reads only RAM (0 up to the RAM size from the hand-off block), the boot flash (`f8180000`, 512 KB) and the
  NVRAM (`f8780000`, 512 KB), at most 64 KB per command. Anything else is refused, so it cannot touch an unmapped
  address or a peripheral register.
- `mw` writes only aligned words in RAM.
- While the console is open, the download server waits; `exit` hands the port back to it.
- `reset` sends the firmware's restart task (REST, `system_restart`) the event its own save-and-restart uses, with
  the restart flags cleared (they would clear the jackpot records or erase NVRAM). It blanks the panel, stops the
  protocols and resets the CPU; our boot loader's window then opens as at power-on. The Studio (Sign window, "Restart
  the sign from its console first") and `tools/serdump.py --reset` use it, so running from RAM or dumping needs no
  power cycle while our firmware runs. Checked by `umami-marquee-sim-test resettest` (NVRAM byte for byte unchanged).

The download task reads the DB9 one byte at a time through `modem_read_char`. The extension takes
that function over. Every byte still goes to the download server, and the console only watches for its magic line.
The download protocol syncs on 0x5A, which the magic does not contain.

## Run from RAM

The serial window (DUMP=1 boot loader) can also take a whole boot-flash image into RAM and boot it, so firmware can
be tried on the sign without writing the chip:
- WRITE puts up to 1 KB at a time into a 512 KB stage at the top of SDRAM (the last 512 KB of the last bank);
  each block comes back with the CRC of what was stored. RUN checks the CRC of the whole staged image and that it
  holds a bootable entry, then the boot goes on from it: the app is loaded from the stage, and its boot volume
  (hand-off RAM 0x2C/0x30) is the stage, so its files (EXT, emoji, fonts, animations) come from the image too. RAM
  0x14 is cut by 512 KB, so the app never uses the stage as RAM.
- Nothing is written to flash, NVRAM or an EEPROM. The window writes SDRAM only inside the stage. A power cycle
  without the host boots the chip's firmware.
- Host side: Umami Marquee Studio, Sign window (pick the DB9 adapter, the image, Run from RAM, then power the sign on), or
  `tools/serdump.py --run IMAGE` (both wait for the window; WRITE frames are sized so no byte is 0x5A).
- Checks: `make -C bootloader runsim` (staging, RUN, bad CRC, writes outside the stage refused, no magic);
  `simulator/tests/test_signlink.sh` (the Studio's C++ sign link against an emulated sign behind a pty).

## Permanent updates (install)

The serial window can also keep an image: INSTALL writes the staged image (checked as for RUN) into the **update
slot**, the last 512 KB of the main flash (erase, program, CRC read back from the flash), and sets the NVRAM boot
count to 0.
- At every start our boot loader tries the slot first, counting the try in NVRAM; the app clears the count once it
  runs (`nvram_downloaded_boot_clear`, after 1200 panel frames). An image that fails its check, or an app that does
  not get that far within a few starts, makes the boot fall back to the boot flash's firmware by itself.
- A slot boot copies the slot into the RAM stage and boots it exactly as RUN does, so the app's boot volume (its own
  files: extensions, emoji, logos, fonts) is that copy in SDRAM. The try is counted with our own flag (NVRAM 0x7FF1
  bit 1, not the original's bit 0); `firmware/ext/slotboot.c` clears the count after 1200 frames, and the 4th try
  parks it at 0xFF (the boot flash from then on, until the next INSTALL or a "retry"). The slot is always left out of
  the app's main-flash download volume, so the app never writes into it.
- Booting the slot in place, with the main flash as the boot volume, crashed on the sign (three resets, watchdog and
  checkstop, then the fall-back): the extension loader reads the boot volume before the firmware has mapped the main
  flash, and the DTLB miss never resolves (docs/BACKLOG.md has the details). The direct mode below keeps that path
  for debugging.
- UNINSTALL erases the slot's first sector: the boot flash boots from then on. The boot flash (the chip) is never
  written over the DB9.
- Host side: Studio, Sign window, Install on the sign / Uninstall; `tools/serdump.py --install IMAGE`,
  `--uninstall` (with `--reset`, no power cycle while our firmware runs).
- Checked by `make -C bootloader installsim` on a main-flash command model: install, boot from the slot, a bad image,
  uninstall, the count given up (0xFF).

## Crash reports

With the extensions (`firmware/ext/crash.c`), a CPU fault (machine check, DSI, ISI, program, FP unavailable,
software emulation) no longer leaves the sign looping silently on the fault: the DB9 gets a report with interrupts
off, then the CPU stops serving the watchdog (with it on, the sign restarts by itself).

```
MQCRASH 1 UM0.1
vector 06 program pc 00064ea0 srr1 00081032 lr 00064e54 dar 00000000 dsisr 00000000
r00 ... r31 (8 per line)
cr ... ctr ... xer ...
task LEDP tcb 07ffe4b0
stack
sp 07ff8ef4 lr 00059a24
...
MQCRASH END
```

The Studio's console (Sign window) decodes it with `analysis/symbols.tsv`: `pc 00064ea0 = rtems_semaphore_obtain+0x94`,
`from 00059a24 = ir_driver_read+0x58`, ... Checked by `umami-marquee-sim-test crashtest` (the simulator now delivers
faults to the firmware's vectors).

## The boot loader over the DB9 (two stages)

So that a boot-loader change no longer means taking the chip out, the boot loader runs in two stages
(`bootloader/include/serdump.h`):
- **Stage 0, the chip:** sets up the clocks, chip selects and SDRAM (the faithful part of the original that does not
  change), then looks in the **loader sector**, the 64 KB of the main flash just below the update slot. With a valid
  stage 1 there (magic, length, CRC), it copies it into SDRAM (0x00F00000) and runs it. Without one it goes on by
  itself, exactly as before.
- **Stage 1, the main flash:** the same boot loader (`make DUMP=1 STAGE1=1 stage1`, `build-dump-s1/stage1.img`):
  window, slot boot, debug record, load and hand-off. HELLO says which one answers: `s=0` the chip, `s=1` stage 1.
- **Safety:**
  - stage 0 counts the tries (debug record `s1_tries`) and stage 1 clears the count at the hand-off. After 2 tries
    that did not get there, stage 0 does everything itself again (until the next INSTALL_S1 or "retry stage 1");
  - with a stage 1 installed, stage 0 first listens 300 ms for **`MQ?DUMP0`**: the chip's own window, and no stage 1
    this boot. That reaches a working boot loader even when stage 1 boots but its window is broken;
  - the loader sector and the update slot are always left out of the app's download volume.
- Host side: Studio, Sign window, **Boot loader (stage 1)**: Install stage 1, Retry stage 1, Remove stage 1 (the last
  two through the recovery window); `tools/serdump.py --install-loader build-dump-s1/stage1.img`,
  `--uninstall-loader`, `--s1-tries 0`, `--recovery` (any other option through the chip's own window).
### Updating stage 0 over serial too (writing the chip)

The boot flash is an 8-bit AMD part on CS0, write-protect clear, so the memory controller lets the CPU write it.
Nothing executes from it once the firmware or a stage 1 is running (the firmware runs from SDRAM; stage 0 copies it
there), so the chip can be rewritten in place over serial with INSTALL_BOOT. The rule that keeps it safe: the boot
loader refuses the write whenever its own code runs from the boot flash (`running_from_bootflash()`), i.e. as stage 0
in place. So it only ever runs from **stage 1** (or a RUN-from-RAM image), whose code is in SDRAM.
- Host: `tools/serdump.py --install-bootflash build-dump/bootflash-fw.bin` (checks HELLO shows `s=1` first);
  Studio, Sign window, Boot loader (stage 1), "Write the chip (stage 0)".
- It erases and programs the 8 sectors one at a time and the sign verifies the whole image by CRC before replying.
- The one risk: a power loss partway through leaves the chip half-written and unbootable until the **programmer**
  rewrites it. That is the only thing the TL866A is still for: the first stage-0 flash on a blank chip, and
  recovering an interrupted chip write. Everything else, stage 1 and the app, is serial-only.
- Checked by `make -C bootloader installsim` (16: stage 0 refuses the write through its recovery window; 17: stage 1
  erases, programs and the sign verifies a new chip image, 8-bit command model).
- Checked by `make -C bootloader installsim` (install, the next boot in stage 1, a slot boot through stage 1, the
  recovery window, tries used up, a bad CRC, uninstall).

## Debugging the boot (NVRAM debug record)

The console and the crash reports only exist once the firmware runs. For what happens before, the boot loader keeps a
**debug record** in the NVRAM's reserved top (0x7C00, `bootloader/include/dbg.h`), which survives resets and power
cycles:
- **The last 8 boots:** each one's reset cause (RSR, cleared at every boot so each shows only its own) and the last
  step it reached: boot loader started, SDRAM up, window closed, slot found / tried / copied, loading, loaded, memory
  map, hand-off; then the firmware's own: started, exception table, main task, memory layout, healthy (1200 frames).
  A boot that stops at "loading" died in the boot loader; one that stops at "memory map" or "handed off" died before
  the firmware's first instruction of ours.
- **The last crash,** with vector, SRR0/SRR1, DAR/DSISR, LR, CR, r0..r31 and the step it happened in, from three
  places:
  - the boot loader itself: once the SDRAM works, stubs at 0x100..0x1400 catch every exception (MSR[IP] cleared).
    Before, an exception once BR0 had moved the boot flash fetched from nothing and checkstopped without a trace;
  - the firmware's DTLB miss handler, whose dead end for unmapped addresses (`b .` at 0x1284, spinning into the
    watchdog) now records the address that missed first;
  - the firmware's crash report (above), which also goes into the record.
- Each crash ends in a watchdog reset; the record says what it was.

Reading it and the switches, all from the boot loader's window (works whatever the firmware does):
- Studio, Sign window, **Debugging**: Boot history, Retry the installed image (try counter to 0), Clear the crash,
  Slot boot: RAM stage / direct, Test fault (the boot loader faults on purpose, to see the record work).
- `tools/serdump.py --boots`, `--tries 0`, `--clear-crash`, `--slot-mode stage|direct`, `--test-fault`
  (add `--reset` while our firmware runs, to get to the window without a power cycle).
- In the firmware's console: `boots` (the same record), `nv OFF BYTE` (a byte in 0x7C00..0x7FFF, e.g. `nv 7ff0 0`).
- The window's HELLO line `dbg=<boot> o=<options> c=<crash vector>` shows it at a glance.
- Checked by `make -C bootloader installsim` (steps, direct mode, OPTION, a trap in the window caught and recorded)
  and `umami-marquee-sim-test dbgtest` (the firmware's steps, the DTLB dead end redirected, a crash recorded).

## What is not there yet

- A GDB stub on the sign (breakpoints and single-step over the DB9). Planned in docs/SIGN-LINK-PLAN.md. Until then:
  the console (md/mw/info), crash reports, and the simulator's GDB server for the same firmware.
- The simulator already has one: VS Code, gdb or Ghidra attach to the emulated board (docs/SIMULATOR-GUI.md).
- Hardware debugging over the MPC862's BDM port needs a BDM pod and the board's BDM header, if it has one.
