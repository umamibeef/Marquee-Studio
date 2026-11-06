# BOOTSTRAP V2.00 rebuilt in C

This is a C reconstruction of the boot loader at the start of the 512 KB boot flash
(AM29LV040B) on the Neurizon CPU-002 board (MPC862P). The original was written by hand in
PowerPC assembly. It passes arguments in registers, keeps r4 = IMMR, and uses almost no stack.
I translated it routine by routine from the disassembly. Each C function names the original
address it came from.

## Build and check

```
make            # build/bootstrap.elf, build/bootstrap.bin (clang + ld.lld, powerpc-unknown-eabi)
make image      # build/bootflash.bin: new bootstrap + all 33 original archive entries, 512 KB
make test       # host test: archive walk + NRV2D rebuild ELITE_RGB byte for byte
make cosim      # emulate original and rebuild side by side, compare all hardware activity
make tables     # regenerate src/tables.c from the dump
make NET=1 image   # build-net/bootflash.bin: the same, plus the Ethernet network window
make netsim        # emulate the NET=1 image with an emulated LAN (DHCP, ARP, ping, TFTP)
```

The build needs the Homebrew `llvm` and `lld` packages. `make cosim` also needs `unicorn` in
`../venv` (`pip install unicorn`).

## Boot sequence

1. Reset (0x100): set the MSR and IMMR = 0xFF000000, then jump to the link address.
   Until BR0 is written, CS0 answers at every address, so this jump works.
2. Basic set-up: watchdog on, debug enable, unlock the keyed registers, clear the boot-info
   block in DPRAM, and map this flash on CS0 at 0xF8180000.
3. Read the board config EEPROM (I2C 0xA0) until its sum and xor bytes check out.
4. Set up memory. The config picks one of two paths:
   - SPD path (`cfg[4] == 1`, `cfg[5] < 0x50`):
     - chip selects sized from board straps (PIPR, PBDAT, PDDAT);
     - SDRAM sized and timed from the module's SPD EEPROM (I2C 0xA2), with one of three UPM programs;
     - a temporary MMU map.
   - Fixed path: chip selects, PLL and UPM program all come from the EEPROMs. The UPM
     program is read from I2C 0xA2.
5. Load the application:
   - The loader first looks for an upgrade image in the main flash (0x50000000), unless the
     NVRAM boot counter at 0xF8787FF0 is 0xFF. Each try increments the counter. The fourth try
     falls back to the boot flash, and after that the upgrade is disabled (0xFF).
     Bit 0 of 0xF8787FF1 tells the app which image it is running.
   - If there is no upgrade, or decompressing it fails, the loader uses the image in the boot flash.
   - A bootable entry has flag bit 0 set and must pass its sum/xor check word. An NRV2D image
     is copied to 0x300000, then decompressed to address 0.
6. Final MMU map of all SDRAM banks. The mappings are also recorded in the app's descriptor
   table, whose pointer and length are at RAM 0x4C/0x50 of the loaded image.
7. Hand-off:
   - Boot info goes to RAM 0x14..0x98: clocks, memory sizes and addresses, bank list, and
     the board ID at 0x98.
   - The MMU and caches are turned on, then the code jumps to address 0.

## Boot flash format

The flash is a chain of entries, and the bootstrap is entry 0. Each entry has:
- `u32 len`, `u32 size`, `u32 flags`, `u8 namelen`, then the name;
- data starting at `(namelen + 16) & ~3`;
- a final check word.

The check word is a CRC-32 for ordinary entries. Bootable entries (flag bit 0) use
`sum16 << 16 | xor16` instead. Flag 0x20000000 means NRV2D-compressed and 0x40000000 means zlib.
The chain ends with a zero word. See `include/archive.h` and `tools/mkimage.py`.

## How it was verified

`make test` runs on the host. Walking the real dump finds ELITE_RGB, and `nrv2d.c` decompresses
it to exactly the 777,716-byte reference. A corrupted check word and a truncated stream are
both rejected. The new `build/bootflash.bin` passes the same test.

`make cosim` runs on an emulated board (Unicorn). Both the original 2003 bootstrap and this build
run from reset to the jump into the app, on the same simulated board (EEPROMs, timer, CPM, straps,
SDRAM, NVRAM, main flash). These scenarios all come out identical:
- fixed path: no upgrade, upgrade, fourth-try fallback, and truncated upgrade with fallback;
- SPD path: one flash bank + CS7, and two banks with an upgrade.

Each run compares:
- the ordered list of hardware register writes (371k to 555k per run, including each watchdog kick);
- every SPR/MSR access;
- the DPRAM state, all 16 MB of SDRAM, and the NVRAM counter.

## Differences from the original

- The code is C compiled by clang, so it is not byte-identical to the 2003 assembly. It is also
  smaller (8,752 vs 9,860 bytes).
- The stack sits at DPRAM 0xFF002C00 instead of the original's 64 bytes below
  0xFF002040. C needs more room, and the region is unused at boot.
- `i2c_write` is included but never called, as in the original, so the linker drops it. The
  original's unused memory-test loop (0xF8180C54) is not reproduced.
- Some quirks are kept on purpose, and each is marked in the code:
  - The archive walk's wrap-around path adds the entry length twice.
  - The name-length and flags loads ignore their own wrap computation.
  - The NRV2D back-reference check compares against output base 0, so it can never fire. A
    corrupt stream would read from wrapped addresses instead of failing cleanly.
  - The fixed path ignores the loader's return value.

## Ethernet network window (`make NET=1`)

The default build is the faithful rebuild. `NET=1` adds a network window, built on:
- an MPC862 FEC driver (`net/fec.c`, polled, MII on Port D, PHY found by scanning MDIO);
- lwIP 2.2.1 (vendored in `../third_party/lwip`, BSD licence), with IPv4, ARP, ICMP, UDP, DHCP and the TFTP server;
- the window logic itself (`net/netboot.c`).

The window runs once SDRAM works and before the app is loaded. The caches are still off at that
point, so DMA needs no cache handling. It uses SDRAM 0x400000 to 0x7FFFFF for:
- a 2.5 MB upload buffer;
- lwIP data;
- the descriptor rings;
- its own stack.

That range is clear of the app (0..~0xBE000) and of the decompression staging area at 0x300000.

### Timeline at boot

- No PHY: no delay.
- PHY but no link: up to 2.5 s.
- With a link: DHCP, then after 3 s the fixed address 169.254.77.1/16. A laptop on a direct
  cable falls back to 169.254/16 too, so the two can talk with no setup.
- Once addressed, the window stays open for 5 s, and for up to 60 s while a transfer runs.

The MAC address is `02:4d:4b` + the low 3 bytes of the board ID (locally administered).

### Using it from a laptop

```
tftp 169.254.77.1
tftp> binary
tftp> get info.txt          # board, clocks, memory map, PHY, address
tftp> get bootflash.bin     # back up the 512 KB boot flash (no programmer needed)
tftp> get flash1.bin        # main flash bank 1; also flash2.bin, nvram.bin
tftp> get eeprom_a0.bin     # board config EEPROM: variant, memory set-up, panel size
tftp> get eeprom_a2.bin     # UPM program (fixed boards) or SDRAM SPD (SPD boards)
tftp> put app.img           # archive with a bootable entry: booted from RAM at once
```
`tools/mkapp.py ELITE_RGB app.img` wraps a raw app binary as an uncompressed bootable entry.
A whole `bootflash.bin` image works as an upload too. **Nothing is ever written to flash.**
A RAM boot does not touch the NVRAM boot counter, and Port D and the FEC are returned to
their reset state before the app starts.

`info.txt` also decodes the config EEPROM:
- the board family (bytes 4/5);
- the panel size the app will use;
- whether the app needs Port D for the panel. If it does, Ethernet can only work during boot.

### Verified in emulation (`make netsim`)

The NET=1 image runs on the emulated board with an FEC/LXT972A model and a scripted host. The test covers:
- DHCP discover, then the static fallback;
- ARP and ping;
- TFTP get of `info.txt`, `bootflash.bin` (byte-identical) and `nvram.bin`;
- TFTP put of the compressed ELITE_RGB entry (fixed path) and of an uncompressed `mkapp.py`
  image (SPD path), each booted from RAM with the app correct in SDRAM.

A board with no PHY sends nothing and boots normally from flash, and `make cosim` confirms the
default build is still identical to the original.

### Hardware unknowns

- Port D wiring. The FEC needs Port D, and the app treats Port D as GPIO on some board variants:
  `FUN_00055acc` checks config bytes 4/5, and on variant 0 the LED panel uses PD9, which is
  FEC TXD0. The bootloader only borrows the pins and gives them back, but the PHY may simply not
  be wired to Port D on this board.
- Untested settings. The PHY address, the MDC clock divider and the FEC set-up values
  come from the U-Boot/RTEMS MPC8xx drivers and are untested here.
- Timer use. The window uses CPM timer 2 as its clock. The bootloader otherwise uses only timer 1.

## Serial dump window (`make DUMP=1`)

`DUMP=1` adds a read-only dump window on the DB9 serial port (`src/serdump.c`, protocol in
`include/serdump.h`). It is for reading out the board when Ethernet does not work. It combines
with `NET=1` and `PATCH=1`, and output goes to `build[-net]-dump[-patch]/`. The Makefile variable
for the original dump is `ORIG` (`make DUMP=<path>` still works for that too).

### When it runs

It runs at the same point as the network window: SDRAM is up, the caches are off, and the app is
not loaded yet. With `NET=1` as well, the serial window runs first and the network window after
it. A fault in the Ethernet path therefore cannot stop a serial dump, and SDRAM is still untouched
when the serial window runs.

### UART

The window uses the same channel, pins and baud-rate generator as the app's `/dev/rs232`
(`serial_pins_config` 0x532D0, tables 0xB94A0 / 0xB9DE8, `smc_uart_init`, `scc_uart_init`,
`cpm_brg_value`):
- default board: SMC1, PB24 SMRXD1 / PB25 SMTXD1, BRG2;
- Port C/B boards (config byte 4 == 2, or byte 4 == 1 and byte 5 >= 0x50): SCC1, PA15 RXD1 / PA14 TXD1, BRG2.

Only the board's own channel is set up, because each channel's pins have a different use on the
other board. The modem-line GPIOs and PA5 stay at their reset state. The UART is polled, 115200 8N1,
with one character per RX buffer descriptor. All BDs and buffers are in DPRAM 0xFF002C00..0xFF002FFF.
The core copies flash, NVRAM and SDRAM data through bounce buffers, so the CPM only reads DPRAM.
On the way out, every register the window changed is put back.

### Timeline

- The window listens for the magic `MQ?DUMP\r` for about 2 s. If it does not come, the boot goes on as before.
- If the magic comes, the sign sends the line `MQDUMP READY 1 SMC1 115200`, then serves requests until BOOT,
  or until 30 s pass with no request.

### Protocol

- Frame: `'M' 'Q' type tag len16 payload crc32`. The CRC-32 (zlib) covers type..payload.
- Request payloads are XORed with the tag. The host picks a tag that keeps 0x5A, the sync byte of
  the app's download frames, out of every byte it sends.
- Requests:
  - HELLO: info text with the variant, clocks, memory map, NVRAM boot tries/flags and the region list;
  - READ region/offset/length: up to 16 KB;
  - CRC: the CRC-32 of any range in a region;
  - SPEED: the sign reports the real rate it can make. If no valid frame comes at the new speed for 3 s,
    both sides go back to 115200;
  - PING;
  - BOOT;
  - WRITE (into region 8, the stage, only) and RUN: run a whole boot-flash image from RAM (docs/SERIAL-DEBUG.md,
    "Run from RAM"). RUN is the only way the window starts code, and only code that passed its CRC check;
  - INSTALL and UNINSTALL: the staged image into the update slot (the last 512 KB of the main flash), or erased
    from it (docs/SERIAL-DEBUG.md, "Permanent updates"). The only flash the window ever writes is the slot.
- A bad frame is dropped, and the host asks again.

### Regions

The region list is an allow-list: 1 boot flash, 2/3 main flash banks, 4 NVRAM, 5/6 EEPROMs 0xA0/0xA2
(read live over I2C, using the boot's own read), 7 SDRAM bank 0. It is the only way to read
memory, and it checks offset and length. Region 7 is speculative: the window does not write
SDRAM, so after a warm reset the bank may still hold what the app left there.

### No writes but to the stage

- The window writes SDRAM only inside the stage, flash only inside the update slot (INSTALL, UNINSTALL), and NVRAM
  only the boot count (INSTALL sets it to 0). No I2C write. The boot flash is never written.
- It starts no code except a staged image that RUN has checked; otherwise it returns to the normal boot.
- `make serdumpsim` checks this on every run.

### Using it from a laptop

Put the DUMP=1 `bootflash.bin` on a **spare** AM29LV040B (keep the original chip), then run:
```
../venv/bin/python ../tools/serdump.py --port /dev/<your-serial-port> [--speed 460800]
# then power the sign on; files go to ../dumps/serial/ (info.txt, *.bin, manifest.txt)
```
- The tool waits for the window and prints the info.
- It dumps eeprom_a0, eeprom_a2, nvram, bootflash, flash1 and flash2. Add `--regions all` for
  sdram too.
- Partial `.part` files resume after a power cycle. The tool first checks them against the sign in
  1 MB pieces.
- It re-checks every finished region by CRC, then sends BOOT.

Dump time for about 33 MB (2 x 16 MB main flash and the rest), wire time only, with BRGCLK =
50 MHz:

| Speed | Time |
|---|---|
| 115200 (really 115740) | about 49 min |
| 230400 (really 223214) | about 26 min |
| 460800 (really 446428) | about 13 min |

The verify pass and per-block turnaround add about 1 to 2 minutes.

### Verified in emulation (`make serdumpsim`)

The UART model covers SMC1/SCC1 at BD level, like the FEC model. Data only passes when the board's
DB9 channel is a working 8N1 UART at the host's speed. Three boards are tested:
- fixed memory with SMC1;
- SPD memory with SMC1;
- fixed memory with SCC1.

On each board, `tools/serdump.py` dumps every region, and each file is byte-identical to the
emulated contents. The boot flash file equals the image, and its archive equals `dumps/bootflash.bin`.

Read-only checks:
- no store to flash or CS7 at any time;
- no store to NVRAM or SDRAM in the window;
- every I2C transaction is a read;
- the CPM only touches DPRAM.

Robustness checks cover corrupted and dropped bytes, bad requests, speed change and fallback,
resume, and the idle timeout.

With no magic, including with line noise on the port:
- the board boots normally after 2.0 s of the window's own clock;
- register/SPR writes, SDRAM, NVRAM and boot info are identical to the default build;
- all registers are put back.

NET=1 DUMP=1 boots both ways, and `make cosim` still shows the default build is unchanged.

### Hardware unknowns

- The window has not run on the board.
- The pin set-up is copied from the app, but only TXD/RXD are driven.
- RBASE/TBASE use the app's convention (the low half of the BD address).
- Baud accuracy depends on the real BRGCLK. The window takes the clock from the EEPROM
  oscillator x PLL MF >> DFBRG, as the app does. The info line reports the rate it really set.
- It is unknown whether the PL2303 can follow the non-standard 223214/446428 rates. If it cannot,
  the tool falls back to 115200.

## Firmware patches (`make PATCH=1`)

`make PATCH=1 image` adds a `PATCH` archive entry and the code to apply it (`src/patch.c`).
- When: after the final MMU set-up (real mode, data cache off).
- What: the bootloader copies the patch code to its load address and writes a list of words into
  the decompressed app.
- Safety: every word is checked against its expected original value first, and nothing is applied
  unless all of them match.
- Current patch: colour emoji fonts (`patch/`, `docs/COLOUR-EMOJI.md`). Superseded by native emoji
  (docs/NATIVE-EMOJI.md); the studio no longer builds fonts for it.
- Tests: `make emojitest` and `test/patchsim.py`.
- It combines with `NET=1`.

## Not yet tested on hardware

Nothing here has run on the real board. The emulation shows the rebuild behaves exactly like
the original *given the board model*. The model is still a simplification: I2C completes at
once, the timer is always due, and the strap and EEPROM values are invented. Before flashing:
- use a **spare** AM29LV040B and keep the original chip;
- program `build/bootflash.bin` with the patched minipro at 3.3 V;
- read it back and compare.
