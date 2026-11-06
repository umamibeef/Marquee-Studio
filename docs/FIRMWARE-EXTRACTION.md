# Extracting the firmware the sign is running (2026-10-31)

This is a planning document. The sign was not available when it was written, so nothing here has been tried on the hardware.
It lists every practical way to get the following from the CPU-002 board:

- the running app;
- the main flash (2x AM29LV128, 32 MB of LFS volumes);
- the NVRAM (32 KB battery-backed SRAM);
- the config EEPROMs (I2C 0xA0 and 0xA2).

It then ranks the methods for someone with a TL866A, a Saleae and no debugger.

Every claim carries one of these evidence tags:

| Tag | Meaning |
|---|---|
| **[code]** | Verified by reading the decompile, the disassembly or our boot loader source. |
| **[emu]** | Verified by running the firmware's own code in Unicorn (`analysis/tools/fw.py`) or by `make netsim`/`cosim`. |
| **[hw]** | Seen on the real board earlier (README captures). |
| **[inf]** | Inferred. Not checked. |

## 0. What "the running firmware" is, and why one look may settle it

- Image choice at boot [code] (`bootloader/src/loader.c`, translated from 0xF8181C18/0xF8181CF8; checked by `make cosim`):
  - BOOTSTRAP V2.00 boots a bootable entry from the main flash when `BOOT_TRIES` (NVRAM 0x7FF0) is not 0xFF
    and such an entry exists.
  - Otherwise it boots ELITE_RGB from the boot flash.
  - It sets `BOOT_FLAGS` bit 0 (NVRAM 0x7FF1) to 1 only when it booted the main-flash image. It clears the bit
    when it falls back.
- How the app reads this [code]:
  - `nvram_downloaded_boot_flag` (0x5F5F8) reads the same bit: the NVRAM byte 0xF below the top.
  - `lfs_find_app_image` (0x5CCC8) uses it to pick volume 1 or 0.
  - `menu_show_app_source` (0x157F4) uses it as the label of menu item 21: `App-Download:` or `App-BuiltIn:`
    (`... Running:` on panels wider than 80 px).
  - The *value* of item 21 is `nvram_boot_flag_get` (0x5F698), that is, whether downloaded boot is enabled (`BOOT_TRIES` != 0xFF).
- If the sign shows `App-BuiltIn:`, the running app is the ELITE_RGB already extracted [code + emu]:
  - The original loader only copies and NRV2D-decompresses it. The boot flash has no patch step.
  - `bootloader/` `make test` decompresses our dump's entry to the exact 777,716-byte reference.
  - Only `.data`/`.bss` change at run time.
- If it shows `App-Download:`, the running app is a bootable entry in the main flash. Then one of the main-flash
  methods below is needed.

Look at the menu before extracting anything. Method M1 costs nothing and may make the rest unnecessary.

## 1. Firmware-side readouts (protocol commands and menu features that give out data)

The V4.20 app has no memory, file or flash read-back command on any port.
- Searched: every caller of `rtems_io_write`, `proto_jwam_send`, `_write_r` and `comms_tx_frame` in
  `elite_rgb_named.c` [code].
- Writers to RS-232 / RS-485: only the functions below. Each sends fixed strings, status bytes or counters.
- The other protocol tasks never transmit: CCOM, MKHN, MKCL, TGSL, TRBO, COUG, XPCL and DPRO.
- No console:
  - `console_set_device_maybe("/dev/rs232")` (0x60754) only stores 0.
  - `console_log` (0x60768) is a single `blr` [code: `app.asm` 0x060768 = `4e800020`].
  - The boot log ("Starting V400", ...) is compiled out. That is why the DB9 shows no text at boot [hw].

### 1a. DB9 download server (`/dev/rs232`, 115200 8N1): only identity and status go out
- Receive state machine `comms_rx_byte_statemachine` (0x33468) [code]:
  - It accepts host sync `0x5A`.
  - It accepts only types 0x10, 0x20, 0x21 (with data) and 0x90, 0xBB (header only).
  - Any other type byte drops back to hunting for sync.
  - There is no read, list or dump type.
- Dispatch `comms_frame_handler` (0x33A08) [code]. The sign can send only:

| Sign → host | When | Payload |
|---|---|---|
| `A5 20 00 0E <hdrCRC> 0D "X<8 hex>.lfs" <dataCRC>` | a 0x20 or 0x10 whose name or length is wrong | the expected file name; the 8 hex digits are the board ID word at RAM 0x98 (`build_expected_lfs_name`, 0x33910) |
| `A5 80 <blk16> <hdrCRC>` / `A5 81 <blk16> <hdrCRC>` | 0x21 data blocks | ack / nak with a block number |
| `A5 BB <p1> <p2> <hdrCRC>` | end of each server pass | status byte (`DAT_000b8688`: 0 ok, 1/2/3 errors) |

- CRCs: `hdrCRC` is CRC-16/XMODEM over `A5 type b1 b2`, big-endian. `dataCRC` is CRC-16/XMODEM over the data.
- The read-only probe `tools/signproto.py whoami` sends a deliberately invalid 0x20 (length 1).
  - [emu] with the firmware's own code (`comms_rx_byte_statemachine(600)`, RAM 0x98 = 0x12345678, `modem_read_char`
    fed from a queue, `rtems_io_write` captured):
    ```
    host: 5a 20 00 01 8b 77 00 00 00          (invalid on purpose)
    sign: a5 20 00 0e 31 3b 0d 58 31 32 33 34 35 36 37 38 2e 6c 66 73 d0 fb   ("X12345678.lfs")
    sign: a5 bb 00 00 58 3d                   (the heartbeat, byte-identical to the one captured on the sign)
    ```
  - The download variables (`0xB8658`, `0xB8668`, `0xB866C`, `0x45C420`) were unchanged afterwards. The state
    machine was back in state 1.
  - So the probe has no side effects in the code paths that touch the download buffer.
  - It reads out only the board ID.
- Heartbeat timing [emu + code]:
  - `download_task` (0x32B18) calls `comms_rx_byte_statemachine(10000)`. Each 500 ms timeout adds 250 to the budget,
    so a quiet pass ends after 40 timeouts = 20 s. Then it sends the `A5 BB` frame.
  - This matches the 20.0 s heartbeat seen on the sign [hw].
  - [inf] A heartbeat implies download mode 0 ("direct"):
    - mode 1 (modem) first blocks in `modem_login` on a semaphore;
    - modes 2 and 4 never run this server.
- **Anything that gets data into the sign is a flash write.**
  - A valid 0x20 → 0x21 … → 0x10 sequence ends in state 0xD, and `download_task` then commits the file to the main flash.
  - Rejected for extraction (see M-R1).

### 1b. RS-485 replies (`/dev/rs485`): status only
[code] These are all the functions that transmit on RS-485. None of them takes an address or a file name from the request.

| Protocol / task | Function | What it can send |
|---|---|---|
| JWAM (Jupiters) | `jwam_reply_8003` 0x31ED8, `_8004` 0x321DC, `_8005` 0x3204C, `_8006` 0x3236C | `03 <lvl addr> <len16> <0x800n> [payload] <CRC-16/KERMIT> 04`, then stuffed by `jwam_escape`. 0x8004/0x8005 = one status byte; **0x8006 = the level's u32 jackpot value** |
| Bally DIU (BDIU) | `protocol_gass_handler_maybe` 0x3B920, `protocol_send_banner_maybe` 0x3BF20 | GASS frames (`gass_frame_finish`, CRC-16/MODBUS, end 0xFF): type 2 → `"GASS 01.01-1"`, type 4 → 4 counter bytes, type 8 → one 0, acks `'A'`; banner `"LED Series 2 Display"` |
| WMS (WSLL) | `wsll_command_handler_maybe` 0x3C480 | HDLC-style `7E … 7E` (escape 7D, reflected CCITT CRC from 0xFFFF). Cmd 0 → `80 00 00 01 00`; cmd 3 → `83 82` + 8 u16 counters (incl. RS-485 overruns); 0x31/0x32/0x40/0x60 → one echo byte |
| Translux "Series 2" (TLUX) | `series2_status_display` 0x365E0 (**misnamed**; see below) | `'C'` → id block + `"Series 2 Display"`; `'E'`, `'I'` fixed bytes; `'1'` quick poll → IMSynch site ID + the string at 0xDCD50; `0x0F 0x37/0x39/0x42` → IMSynch download-session status, including the **indices** of LFS entries that failed their check (`lfs_bad_get`) |

- A second download channel, **also a flash write**: the TLUX handler has its own RS-485 file download [code].
  - It is active when the download mode word `0xDCD48` is 2. [inf] That is the "IMSynch" mode, by
    `menu_download_mode_submenu`.
  - Frames `0x0F 0x36` allocate, `0x38` add an entry (with `eclipse_signature_check`), `0x43` store 250-byte blocks
    (`download_block_store`) and `0x41` commit.
  - It only receives files. Rejected for extraction (M-R2).

### 1c. On-panel readouts (IR remote menu): zero-risk, read by eye
[code]. The tree is from `docs/MENU.md`. The PIN levels are in ANALYSIS.md. [inf] The level-3 code [redacted] assumes the
remote's buttons index the decoder's character map directly.

| Item | Function | Shows | View level |
|---|---|---|---|
| 1 | `menu_show_version_maybe` 0x14D94 | the version string compiled into the running app ("V4.20" in ours) | 1 |
| 21 | `menu_show_app_source` 0x157F4 | label `App-Download:`/`App-BuiltIn:` = the image the boot loader started; value = downloaded boot enabled? | 1 |
| 19 | `Storage:` | storage status text | 1 |
| 29 | `View LFS:` → `menu_lfs_file_list` 0x260E4 | for each LFS file: `BT File No.` (boot flash) or `DL File No.` (main flash), name, `Link Name`, `Raw Size` | 2 |
| – | `menu_check_crcs` 0x26784 | "Checking CRCs" over the volumes (`lfs_boot_check_menu` for bootable entries) | 2 |
| DIAG | `diag_memcheck_screen` 0x1281C | "ROM Check-Ok", "RAM Check-Ok" | 2 |

**Do not change item 21, and do not exit the menu with changes.**
- `menu_load_values` (0x28090) only reads the value.
- The apply path in `config_groups_load_all` (0x28BB8, around 0x29C60) calls `nvram_boot_flag_set` (writes
  `BOOT_TRIES`) when item 21's value differs from NVRAM, and `nvram_downloaded_boot_clear` [code]. That is an NVRAM write that changes which image boots next time.
- Also leave `RESET NVRAM:` (item 13) and `Clear Download` (item 158) alone.

### 1d. Naming fixes found along the way (suggested, not applied here)
- `comms_tx_raw` (0x3413C) does not transmit. It appends received 0x21 block data to the download staging buffer
  at `_DAT_0045c420`, bounded by `DAT_000bdb44`, and counts bytes. Suggested name: `download_buffer_append`.
- `series2_status_display` (0x365E0) is the TLUX frame dispatcher. It is called only from `proto_tlux_task`. It
  includes the IMSynch RS-485 download. Suggested name: `tlux_frame_dispatch`.
- `0xDCD48` is the live download mode (0 direct, 1 modem, 2 IMSynch, 4 buffer-clear prompt, by `download_task` and
  `menu_download_mode_submenu`) [inf for 4]. Suggested name: `download_mode_live`.

## 2. Methods

### M1. Read the answer off the panel (menu items 1, 21 and 29), plus the optional DB9 `whoami`
- Extracts: which app is running and its version; the LFS file list of both volumes (names and sizes); the
  board ID (from `whoami`). No bytes of firmware.
- How:
  1. Enter the menu with the IR remote and a PIN. Level 1 is enough for items 1 and 21; item 29 needs level 2.
  2. Read item 1 and item 21.
  3. Open `View LFS`. Write down every `DL File No.` entry. A bootable app in the main flash would be listed there.
- If item 21 says `App-BuiltIn:`, the running app = `analysis/extracted/ELITE_RGB` [code + emu, section 0].
- Needs: the IR remote and the PIN, or the level-3 code (button mapping [inf]). For `whoami`: the PL2303 adapter and `signproto.py`.
- Risk: none if you only look [code: viewing alone does not write; the writes are on changed values].
  `whoami` is read-only [emu].
- Effort: 15 minutes.
- Confidence: high that the label reflects the boot loader's choice [code, both sides]. Medium on getting into
  the menu (remote and PIN unknown).

### M2. Network boot loader (`make NET=1`) on a spare AM29LV040B, then TFTP
- Extracts: everything except live RAM:
  - `bootflash.bin` (512 KB), `flash1.bin` and `flash2.bin` (main flash, 16 MB each);
  - `nvram.bin` (32 KB, including `BOOT_TRIES`/`BOOT_FLAGS` at 0x7FF0/0x7FF1);
  - `eeprom_a0.bin` and `eeprom_a2.bin` (256 B each);
  - `info.txt`.
  - Then extract the bootable entry from `flash1.bin` with the boot-flash archive walker
    (`tools/extract_bootflash.py`-style walk; the same entry format as the boot flash [code: `lfs_find_in_volume`]),
    and decompress it with `bootloader/src/nrv2d.c`. Together with `nvram.bin` (bit 0 of 0x7FF1), that tells you
    exactly which image ran.
- How (`bootloader/README.md`):
  1. `make NET=1 image`.
  2. Program `build-net/bootflash.bin` into a **spare** AM29LV040B with the patched minipro.
     The chip exists in the minipro DB as `AM29LV040B@PLCC32` [verified by `minipro -q tl866a -L`].
     The 3.3 V patch is needed (see `tools/dump_bootflash.sh`).
  3. Read the spare back and compare.
  4. Swap it into the PLCC socket. Keep the original chip safe.
  5. Connect a laptop by Ethernet and power on.
  6. `tftp 169.254.77.1`, `binary`, then `get` each file.
- Timing:
  - The window opens after DHCP times out (3 s) and stays open 5 s.
  - Every TFTP request or block extends it by 60 s (`last_activity` is updated in `t_open`/`t_read`) [code].
    So a 16 MB transfer cannot be cut off half way.
  - 16 MB = 32,768 512-byte blocks. That is under the 16-bit block-number limit [code: lwIP `blknum` u16].
- Needs:
  - a spare AM29LV040B-90JC (PLCC32) and the TL866A with its PLCC32 adapter;
  - a PLCC extractor;
  - a laptop with Ethernet and a TFTP client.
- Risk to the sign:
  - **No flash write** [code: `t_read` only reads; `put` goes to SDRAM 0x400000+].
  - NVRAM gets the same `BOOT_TRIES` bump the original loader makes on every boot [code], no more.
  - Do not use `put` (not needed). If you do, it boots from RAM and clears `BOOT_FLAGS` bit 0 [code].
  - Worst case: the image does not run on real hardware. Put the original chip back. The socket is the only wear.
  - [inf] The FEC borrows Port D during the window. On board variant 0 the panel uses PD9 (FEC TXD0), so the panel
    may glitch during the window. The pins are returned afterwards.
- Effort: 1 to 2 hours, given a spare chip.
- Confidence: medium.
  - Verified [emu] (`make netsim`): DHCP, the static fallback, ARP/ping, `info.txt`, `bootflash.bin`
    (byte-identical), `nvram.bin`, `eeprom_a0.bin`, `inputs.txt`.
  - Not exercised by netsim: `flash1.bin`/`flash2.bin`. Checked by reading `test/netsim.py`; it only uses the same `t_read`.
  - Unknowns [inf]: whether the PHY is wired to Port D on this variant, the PHY address, the MDC divider.
  - If the PHY never links, the loader boots normally with no delay [code/emu].

### M3. Serial dump boot loader on a spare AM29LV040B, over the DB9 (to be written)
The same idea as M2, but the data goes out of the DB9 at 115200. This code does not exist yet.
- Why it is worth writing: the DB9 path is the one part of the board known to work.
  - The sign's heartbeat proves SMC1 TX, the ADM211 and the adapter wiring [hw].
  - It does not depend on the unknown Port D / PHY wiring.
- Extracts: the same as M2. A useful saving: walk the LFS chain (`lfs_chain_end_maybe`-style) and send only the used
  part of each bank, with a simple RLE for 0xFF runs.
- How [inf]:
  1. Add a polled SMC1 UART (or SCC1 on boards with `board_panel_lines_off_portd`, decided from config EEPROM
     bytes 4/5 as the app does [code: `serial_dev_names_alt`]) to the C boot loader.
  2. Wait about 2 s for a key. If it comes, stream a framed, CRC-32-checked dump.
  3. Otherwise boot normally.
  4. Best: build it into the NET=1 image, so one chip tries Ethernet first and serial second.
- Speed [inf]: 115200 8N1 is about 11.5 KB/s.
  - Both full banks take about 49 minutes; the used part is likely much less.
  - [inf] The ADM211 is rated for about 115 kbps, so do not count on going faster.
- Needs: the same spare chip and TL866A as M2, plus the PL2303 adapter.
- Risk: the same as M2. Read-only by design. Booting a new loader is the only risk.
- Effort: about a day. Pin setup can be copied from the app's `smc_uart_init` / the emulated cosim board.
  Test in emulation like `make netsim`.
- Confidence: medium-high once written. Few hardware unknowns: SMC1 pins and BRG divider.

### M4. Saleae I2C capture of the EEPROMs at power-on
- Extracts: both config EEPROMs (256 B each as the boot loader reads them). Not firmware, but needed to rebuild the board state.
- How: clip SDA/SCL/GND on the 8-pin 24Cxx (BACKLOG item 1: pin 4 GND, 5 SDA, 6 SCL [inf from the part type]),
  run the I2C analyser, and power on.
  - The boot loader reads `i2c_read(0xA0, cfg, 256)` until the sum/xor checks pass.
  - It reads `i2c_read(0xA2, …, 256)` for the UPM program (fixed path) or the SPD (SPD path) [code: `src/main.c:38,104`, `src/sdram.c:26`].
- Needs: the Saleae and fine clips.
- Risk: none (passive).
- Effort: 30 minutes.
- Confidence: high [code]. M2 also returns the same 256 B buffers.

### M5. DIY BDM through the 2x8 DEBUG PORT
- Extracts: anything, including live SDRAM. This is the only method that reads the code *as it is executing*,
  together with the heap, jackpot state and IMMR.
- How [inf]:
  - MPC8xx BDM is a synchronous serial port (DSCK, DSDI, DSDO, plus HRESET/SRESET and VFLS0/1 for freeze status).
  - It is described in the MPC862 user's manual, development-support chapter.
  - The Saleae survey already suggests the roles [hw/inf]:
    - inputs at 0.27 V on pins top#4, top#6, bottom#7 (debugger → CPU);
    - outputs top#5/top#7 high in reset (VFLS-like);
    - the 74HCT125 fixes the direction.
  - Find the pinout by tracing the header to U7 and the resistor packs.
  - Then bit-bang from a microcontroller (a Pi Pico), or buy a period BDM probe (Wiggler, P&E or BDI2000 class [inf]).
  - Freeze the core. Read memory word by word.
- Needs: a microcontroller or a probe, a multimeter, a lot of time.
- Risk: low to medium. Wrong pin drive through the buffer; holding the CPU in reset. No flash write unless you issue one.
- Effort: days to weeks.
- Confidence: medium-low until the pinout is known.

### M6. Warm-reset SDRAM dump through the network window (extension of M2; speculative)
- Idea: the NET window runs before the app is loaded [code: `src/main.c:152-172`].
  - The boot loader does not clear SDRAM [code: the original's memory test is unused and not reproduced].
  - The window uses only 0x400000 to 0x7FFFFF [code].
  - After a reset without power loss (P1 "Hard reset" pads or the LTC695), 0x0 to 0x3FFFFF may still hold the previous
    run's decompressed app and its data.
- Change needed: a `get sdram_lo.bin` (0x0 to 0x3FFFFF) file in `net/netboot.c` (about 5 lines; not written).
- Value: checks the running code (`.text`) against the flash image without a debugger.
- Risk: as M2.
- Confidence: low [inf]. SDRAM is not refreshed during the reset pulse (about 200 ms). Retention at room
  temperature is usually longer, but this is not guaranteed.

### M7. Chip-off main flash, read in another programmer
- Extracts: the main flash, bank by bank.
- TL866A: not possible.
  - minipro 0.7.4's database has no AM29LV128 entry and no TSOP56 parts [verified: `minipro -q tl866a -L 29LV128`,
    `-L TSOP56` return nothing].
  - A 128 Mbit x16 part needs more than the 40 ZIF pins [inf].
- Would need [inf]: a T48/T56-class programmer with a TSOP56 adapter, or a home-made reader. Plus hot-air rework of two TSOP56 parts.
- Risk: high. Lifted pins or pad damage on a 2003 board. No firmware risk.
- Effort: high.
- Confidence: medium.

### M8. In-circuit bus reading (Saleae or a microcontroller on the flash pins)
- Ideas [inf]:
  - Sniff the copy loop while the boot loader reads an upgrade image;
  - or hold HRESET and drive the flash directly.
- Why it is impractical [inf]:
  - The main flash is on a 32-bit bus. That is far more than 16 Saleae channels, and there is no free strobe channel.
  - MPC862 pin states in reset are not guaranteed high-Z.
- Not recommended.

### Rejected
| ID | Method | Why rejected |
|---|---|---|
| M-R1 | DB9 download protocol "read" | No such command [code, section 1a]. Getting a dumper in through it needs a valid 0x20/0x21/0x10 transfer, which **writes the main flash**. |
| M-R2 | IMSynch download over RS-485 (TLUX 0x0F 0x36/0x38/0x43/0x41) | Receive-only, and a commit **writes flash** [code, section 1b]. |
| M-R3 | Download a "dumper app" as a `.lfs` and let the boot loader run it | Needs M-R1 or M-R2: a **flash write**, and it changes `BOOT_TRIES`. M2/M3 do the same from RAM with no write. |
| M-R4 | Original BOOTSTRAP V2.00 console or monitor | None exists. Its only peripherals are I2C, memory controller, MMU, watchdog and timer. Its only strings are "BOOTSTRAP V2.00" and "ECLIPSE". The C rebuild has no UART code, and `make cosim` matches the original's full ordered list of hardware register writes [code/emu]. Power-on key and BREAK tests got nothing [hw]. |
| M-R5 | App boot console | `console_log` is a `blr` [code]. |
| M-R6 | Desolder the NVRAM SRAM | **[inf]** The photo shows a separate coin cell (B1), so the SRAM is an ordinary chip kept alive by the board. Removing it loses its contents. Use M2/M3. |
| M-R7 | Ethernet on the stock app | The app has no network code [code: ANALYSIS.md]; the port is silent [hw]. |

## 3. Ranked recommendation (TL866A + Saleae, no debugger)

| Rank | Method | Gets | Flash write? | Effort | Confidence |
|---|---|---|---|---|---|
| **1** | **M1** menu items 21/1/29 (+ `whoami`) | which app runs, version, LFS list, board ID | no | minutes | high |
| **2** | **M2** NET=1 loader on a spare chip, TFTP | main flash, NVRAM, EEPROMs, boot flash | no | 1–2 h | medium |
| **3** | **M3** serial-dump loader (write it; ideally in the same image as M2) | same as M2, slower | no | ~1 day | medium-high |
| 4 | M4 Saleae I2C at power-on | EEPROMs | no | 30 min | high |
| 5 | M6 warm-reset SDRAM dump (M2 + 5 lines) | running `.text` from RAM | no | +1 h | low |
| 6 | M5 DIY BDM | everything, live | no | days–weeks | medium-low |
| 7 | M7 chip-off main flash | main flash | no (but rework risk) | high | medium |
| – | M8, M-R1..M-R7 | – | – | – | rejected or impractical |

Suggested order once the sign is available:
1. Clip the Saleae on the EEPROM (M4) and capture the first power-on.
2. Use the remote to read items 1, 21 and 29 (M1). Change nothing. Optionally run `signproto.py whoami`.
3. If item 21 says `App-BuiltIn:`, the running app is already in hand. M2 is then only needed for the main-flash
   content (themes, fonts, animations) and the NVRAM.
4. If it says `App-Download:`, or you want the content anyway, use M2. If Ethernet does not come up, use M3.
   Before then, build one image with both, so only one chip swap is needed.
5. **Keep the original AM29LV040B untouched throughout.** Its dump is `dumps/bootflash.bin`
   (sha256 `484bbad2…91cd`).

## 4. Reproducing the emulation check
The `whoami` result in section 1a came from a throwaway script (not in the repo). It:
1. loads `analysis/tools/fw.py`;
2. sets RAM 0x98 = 0x12345678 and calls `build_expected_lfs_name`;
3. hooks `modem_read_char` to return the bytes of `signproto.frame(0x20, b"\x00")`, then -1 (timeout);
4. hooks `rtems_io_write` to record each buffer (args at r5: `{ptr, len}`);
5. calls `comms_rx_byte_statemachine(600)`.

It printed the two sign frames shown above. The download-buffer variables did not change.
