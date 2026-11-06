# Flashing the from-source firmware (boot flash, AM29LV040B)

The stock ELITE_RGB V4.20 app lives in the socketed boot flash, an AMD AM29LV040B (PLCC32, 512 KB, 3.3 V). It is
stored NRV2D-compressed next to the boot loader and the content (fonts, animations, logo). This page makes a
complete boot-flash image in which ELITE_RGB is our from-source build, and puts it on a **spare** chip.

## 1. What the image contains

`make -C bootloader fwimage` writes `bootloader/build/bootflash-fw.bin` (512 KB). It contains:
- the original boot loader, byte for byte from your dump (`--stock-boot`), so only the app changes;
- ELITE_RGB built from source (`analysis/tools/fwbuild.py`), compressed with `bootloader/tools/nrv2d_pack.py`. The
  compressed stream is decompressed again with the boot loader's own decoder (`bootloader/src/nrv2d.c`) and
  compared before the image is written;
- every other entry of your dump, unchanged: theme, logo, the 20 effects, the 8 fonts;
- with the extensions (the default): the app is `ELITE_RGB-ext`, the from-source build plus a small loader, and two
  more entries: `EXT`, the extensions' code (native emoji, docs/NATIVE-EMOJI.md, and the serial console,
  docs/SERIAL-DEBUG.md), and `emoji.bin` (15.8 KB). If the `EXT` entry is missing or does not match the app, the
  loader puts back the original instructions and the app runs as the plain from-source build. `NATIVE_EMOJI=0`
  builds the plain from-source app instead.

`make -C bootloader DUMP=1 fwimage` writes `bootloader/build-dump/bootflash-fw.bin`: the same, but with our rebuilt
boot loader and its serial dump window in place of the original boot loader (docs/SERIAL-DEBUG.md).

Without `--app`, the same tool reproduces your dump byte for byte, which checks the image builder itself.

## 2. Build and test (no hardware)

From the repository root:

```sh
(cd analysis/tools && ../../venv/bin/python rebuild.py --trials 24)   # replace and verify the C functions
(cd analysis/tools && ../../venv/bin/python fwbuild.py)               # link ELITE_RGB from source; must print IDENTICAL
(cd analysis/tools && ../../venv/bin/python fwext.py)                 # add the extensions (native emoji): ELITE_RGB-ext
make -C bootloader fwimage PYTHON=../venv/bin/python   # with native emoji; NATIVE_EMOJI=0 for the plain app
make -C bootloader fwtest  PYTHON=../venv/bin/python   # must print RESULT: PASS
```

`fwtest` (`umami-marquee-sim-test flashtest`, the C++ simulator, built on first use; formerly `sim/flashtest.py`) boots the
whole image in the system simulator, as the sign would:
1. the boot loader in the image finds and decompresses ELITE_RGB. RAM must then hold the from-source build byte for
   byte, except the hand-off block and the MMU table the boot loader writes on purpose;
2. the app runs its tasks (INIT, LEDP, REST, SMOD, the random task) and drives the LED panel. Its sequence of
   distinct panel frames must equal the stock dump's.

Result (2026-11-01): PASS, with 48 of 48 distinct frames identical and the same five tasks.

Longer checks on the same build (`sim/compare.py`, `sim/compare_rs485.py`, docs/REBUILD-PLAN.md 4d and 4e). They now
run in C++, with the same arguments, as `build/simulator/umami-marquee-sim-test compare`, `compare-rs485`, `compare-db9` and
`compare-menu [--edit]` (docs/SIMULATOR-GUI.md):
- Over about 3,250 panel frames: 95 of 95 distinct frames identical, the same tasks, normal display then attract
  at the same points.
- An RS-485 Cougar jackpot record (level map configured) is parsed, matched to its level and stored exactly as by
  the stock firmware: the same `jackpot_location_update` call, the same jackpot records in NVRAM, the whole NVRAM
  identical, and 95 of 95 frames identical.
- The DB9 download port (`sim/compare_db9.py`) gives the same replies to read-only probes, byte for byte.
- The remote-control menu (`sim/compare_menu.py`): 20 navigation keys, decoded by the app's own IR interrupt
  handler, give the same display states and 42 of 42 distinct menu frames identical.
- A menu edit saved to NVRAM (`sim/compare_menu.py --edit`): after "Display No." is changed and confirmed, 79 of 79
  frames are identical, and the whole 32 KB NVRAM byte-identical afterwards.
- The 3 start-up frames that differ in the long run are the progress bar (panel rows 12..15) caught at another
  point. The stock image produces exactly our sequence when only its CPU load changes (docs/REBUILD-PLAN.md 4f).

## Which image to program

| Image | Boot loader | Use it for |
|---|---|---|
| `bootloader/build/bootflash-fw.bin` (`make -C bootloader fwimage`) | the original, from your dump | the from-source app with native emoji and the serial console, with the least change: only the app and two new entries differ from the stock chip |
| `bootloader/build-dump/bootflash-fw.bin` (`make -C bootloader DUMP=1 fwimage`) | our rebuilt one, with the serial window | **the one to flash**: the from-source app (version UM0.1) with native emoji, the console and crash reports, the Umami Marquee start-up animation, and the serial window: run a firmware image from RAM from the Studio (no flash writes), and read out the main flash, NVRAM and EEPROMs (docs/SERIAL-DEBUG.md). Checked in the simulator, including with this sign's own config EEPROM; it has not run on the sign yet |

With the DUMP=1 image on the chip, later firmware is tried by running it from RAM (Studio > Sign > Run from RAM)
and only burned onto the chip once it is good, so the chip is rarely reprogrammed.

### Flashing the original chip (no spare)

The dump `dumps/bootflash.bin` (SHA-256 `484bbad2…91cd`, two identical reads) is the way back; keep a second copy
outside the repository. Then, one step at a time:
1. read the chip and compare it with the dump (`-r`, then `cmp`): this proves the seating, the programmer and the
   backup before anything is erased; stop if they differ;
2. check the programmer's VCC (below): the chip is the only one, so the 3.7 V of the TL866A matters more than ever;
3. write the image (`-w`, which verifies), read it back and compare;
4. fit the chip and power on: the Umami Marquee animation, then the normal display;
5. if anything is wrong: write `dumps/bootflash.bin` back the same way and compare.

## 3. Program a spare chip

> **Safety rules**
> - Use a **spare** AM29LV040B. Keep the original chip out of the programmer. It is your only copy of the stock
>   boot flash (its dump is `dumps/bootflash.bin`).
> - Use the **patched** minipro in `build/install/bin/minipro`. Stock minipro 0.7.4 drives 3.3 V chips at 5 V on a
>   TL866A (upstream issue #387) and can destroy the chip.
> - The main flash on the board is never written by any of this.

> **Check the supply voltage before writing (review, 2026-11-01).**
> - The patched minipro sends the database's VCC code for this chip (2 = 3.3 V) instead of 5 V.
> - On the TL866A, though, VCC was measured at **about 3.7 V on the empty adapter** (README, "Boot flash dump").
>   That is above the AM29LV040B's 2.7 to 3.6 V operating range (absolute maximum 4.0 V). Reading worked at that
>   voltage, but erasing and programming take far longer and draw more current.
> - Prefer a **T48 or TL866II+**: their minipro driver passes the database voltages through and was never affected.
> - If you use the TL866A: measure VCC (adapter pin 32 to pin 16) with the **spare** chip fitted, during a read
>   (`tools/tl866_voltcheck.py`), and program only if it stays at or below 3.6 V.
> - Pass no voltage options; the database values are right for this chip. (The patched build now handles `--vpp` and
>   `--vcc` correctly on the TL866A: `tools/minipro/tl866a-voltages.patch`.)

Flags from the patched minipro's manual page (`build/install/share/man/man1/minipro.1`): `-w FILE` writes, then
verifies unless `-v` is given; `-m FILE` verifies only; `-r FILE` reads.

From the repository root:

```sh
M=build/install/bin/minipro
CHIP="AM29LV040B@PLCC32"
$M -p "$CHIP" -w bootloader/build/bootflash-fw.bin     # erase, write, verify
$M -p "$CHIP" -r dumps/fw-readback.bin                  # read it back
cmp dumps/fw-readback.bin bootloader/build/bootflash-fw.bin && echo "readback OK"
```

If minipro reports an ID mismatch, the chip is not seated correctly. Reseat it and try again. Do not override the
ID check.

## 4. On the sign

1. Power off. Fit the programmed spare chip in the boot-flash socket (PLCC32; note the pin-1 corner).
2. Power on. With the DUMP=1 image: the Umami Marquee start-up animation with "UM0.1" in the corner (the stock image:
   the Mikohn logo and "V4.20"), then the normal display.
3. To go back, power off and refit the original chip.

This build has not yet run on the sign. The checks still to do on the sign are listed in `docs/BACKLOG.md`.

## 5. Limits of the verification

- The simulator models the board: CPU, interrupts, timer, the CPM serial/I2C/IDMA, the panel output, the config
  EEPROMs. Real-hardware timing and the FPGA itself are not modelled.
- Compared so far: start-up, all tasks, the boot animation, normal and attract display, RS-485 frame reception,
  the DB9 download port's replies, and the remote-control menu (navigation).
- Not yet compared in the simulator:
  - a stored jackpot drawn on the panel (needs the sign's pool/theme set-up from its NVRAM);
  - audio output (the AC-link is kept as the original code).
- 602 of about 1,170 app functions run as our compiled C. The rest is the original code, reassembled from our
  assembly source byte for byte (`analysis/tools/asmsrc.py`).
