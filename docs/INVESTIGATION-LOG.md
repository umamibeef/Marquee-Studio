# Investigation log

This log records what was tried on the sign before the firmware was dumped, and what the first analysis found.
Later work superseded parts of it; those places are noted inline. The current overview is the README; the protocols are in
`analysis/PROTOCOL-RS485.md`, `docs/CONTROLLER-MESSAGES.md` and `SPEC.md`.

## Probing the ports and headers

### Ethernet: silent
- The link comes up at 100baseTX full-duplex.
- The sign sends nothing at boot: no ARP, DHCP/BOOTP or TFTP (`captures/network/boot-capture.pcap`).
- An ARP sweep of 192.168/16, 172.16/12, 169.254/16 and 10/8, including RFC 5227 probes,
  got zero replies (`captures/network/arp-sweep.txt`).

### DB9 serial: framed binary protocol
- The USB-serial adapter (PL2303GC) has a female DB9 with crossed (DCE-style) wiring,
  so it plugs straight into the sign. **Don't use a null modem.**
- Speed: 115200 8N1 at RS-232 levels (about ±9 V), measured with a Saleae Logic Pro 16.
- Heartbeat: after 20.0 s of silence on the line, the sign sends:
  ```
  A5 BB   00 00   58 3D
  sync    body    CRC-16/XMODEM (poly 0x1021, init 0) over sync+body, big-endian
  ```
- Any byte we send resets that 20 s timer, so the sign's receive path works.
- The sign never raises its DTR or RTS.
- Tried, with no reply on serial and no change on the display:
  - Hayes modem replies (OK/RING/CONNECT) at 2400 to 115200.
  - Echoing the heartbeat back.
  - `A5 BB XX 00` for XX = 01 to FF.
  - `A5 BB 00 01 XX` for XX = 00 to FF.
- No boot console on this port. Interrupt keys (CR, space, Ctrl-C, Esc, *, #, ?, LF) every 50 ms
  plus a BREAK every 2 s during power-on got no reaction. A single `6B` byte at power-on is a
  driver power-up glitch: it also appears when we only listen.
- Replying within about 1 ms of a heartbeat (echo, `BB A5` sync, bodies 00 01 / 01 00 / 80 00 / 00 80 /
  06 00 / 00 06, a bare 0x06, and a little-endian CRC): no change; the next heartbeat always came 20.017 s later.
- Our DTR/RTS (the sign's DSR/CTS) off, each one alone, both on, and DTR toggling: no change.
- The sign's DCD (pin 1) and RI (pin 9) are not driven by the adapter and have not been tested.

### DEBUG PORT (2x8) at power-on
A Saleae capture on all 16 pins (`captures/saleae/debugport-*`) shows no serial data and no clock on any pin.
After removing sub-100 ns glitches, only single level changes remain:

| Ch | Position | After boot | Behavior | Guess |
|---|---|---|---|---|
| 12 | bottom #5 | 5.0 V | rises at power-on (3.3146 s) | 5 V supply |
| 13 | bottom #6 | 3.3 V | rises at power-on | 3.3 V supply or pull-up |
| 0, 11 | top #1, bottom #4 | 3.3 V | rise 192 ms after power-on (3.507 s) | reset lines released (LTC695 ~200 ms) |
| 4, 6 | top #5, top #7 | 0 V | high during reset, low at reset release | CPU status/freeze outputs? |
| 2, 8 | top #3, bottom #1 | 0 V | high from power-on, low 400 ms after reset (3.909 s) | driven by boot code |
| 3, 5, 14 | top #4, top #6, bottom #7 | 0.27 V | steady | undriven inputs (debugger -> CPU)? |
| 1, 7, 9, 10, 15 | top #2, top #8, bottom #2, #3, #8 | 0 V | never change | ground or unconnected |

This is consistent with a buffered BDM/JTAG-style debug interface that stays quiet until a debugger drives it.

### Jumpers
SW1 alone, SW2 alone, and both together: no change on serial, the LEDs or the display.

What the firmware says (2026-10-31; full notes in `analysis/ANALYSIS.md`, "JP2 SW1/SW2"):
- They are not the menu keys. The on-screen menu is driven by an infrared remote (`/dev/ir`).
- They are most likely two of the board's four switch inputs, one of them a production-test jumper. The app's I/O
  code defines four active-low inputs, in0..in3 = PD8 (PB16 on Port C/B boards), PC14, PA10 and PA11.
  - PA10 (PB16 on Port C/B boards) is the production-test jumper. When it is closed, the diagnostics sequence
    runs its 18-step "Production" test set instead of the 3-step "LED Display" set.
  - Diagnostics run at boot if a config flag is set, or on protocol command 3 from a controller.
  - This explains why shorting SW1/SW2 during normal running changed nothing.
- Other candidates: reset-time straps (the bootloader samples PIPR, PBDAT and PDDAT/PD8 on SPD boards), or
  pure hardware options (FPGA mode, flash write protect, watchdog disable).
- How to tell: boot the NET=1 bootloader and `get inputs.txt` with each jumper open, then closed. The window
  stays open a minute after every request. Or meter the JP2 pins: a ground pin plus a pulled-up pin means a logic input.

## Boot flash dump
Firmware version: V4.20 (version string at 0xBDA90, used by the boot and protocol code; the "Starting V400" boot banner is stale).

`dumps/bootflash.bin`: 524,288 bytes, SHA-256 `484bbad2aa3d925c43f2f33d187466fb53d659388946c03ffebb33422b3591cd`.
- Read 2026-10-30 with the patched minipro on the TL866A (VCC about 3.7 V on the empty adapter).
- Chip ID 0x014F; two identical reads; `tools/check_address_lines.py` confirms every address line toggles.
- Earlier reads in `dumps/raw/` are INVALID: A7 (pin 5) was not making contact, so each byte was duplicated 0x80 apart.
  Some also had A11 dropouts.
- Contents: header `00 00 26 84 00 00 26 80 ...` then a length-prefixed "BOOTSTRAP V2.00" at 0x0D, and "ECLIPSE".
  Mostly compressed data with file names: logo_s.flc, logo_s.wav, logo_e.flc, effect00-19.flc, fonte01-08.png
  (9 PNG headers), plus "JACKP", "RNGdraw", "C:\AAPRO\RGBFX". So it holds content and resources as well as a bootloader.
- The chip went back in the sign (pin-1 dot at the middle of the top edge).

## Firmware analysis (2026-10-30)
Tools: `tools/extract_bootflash.py` (archive walker plus zlib/NRV2D unpacking), `analysis/` (disassembly, extracted files).
- Boot flash layout: 0x00 header (0x2684, archive offset 0x2680, "BOOTSTRAP V2.00"); 0x100 bootstrap PowerPC code;
  0x2680 archive of 33 entries: `u32 ?chk, u32 entry length (incl. 16-byte header), u32 size, u32 flags, u8 name len, name, pad, data`.
  Flags: 0x40000000 zlib; 0x20000001/0x20000000 UCL NRV2D (8-bit bit buffer, bootstrap routine 0xC90, loader 0x1CF8
  unpacks to RAM address 0); 0 stored.
- Files: `ELITE_RGB` (main program, 777,716 bytes, loads at 0, starts `b 0x4000`, "APP ENTRY", "Starting V400");
  `theme` ("Download,Mode=direct / Download,Datarate=115200"); logo_s/logo_e.flc (80x16 animations: the content was made for 80x16;
  the physical panel size is not confirmed); logo_s.wav; effect00-19.flc; fonte01-08.png.
- Serial protocol (receive parser at 0x33468, frame handler 0x33A08, CRC table 0xB328C):
  - Sign to host: `A5 <type> <p1> <p2> <CRC hi lo>`. Host to sign uses sync 0x5A: `5A <type> <b1> <b2> <hdrCRC hi lo> [data] [dataCRC hi lo]`.
  - hdrCRC = CRC-16/XMODEM over `5A type b1 b2` (the parser starts at 0xFBBF = CRC of 0x5A). dataCRC = CRC-16/XMODEM over the data (init 0).
  - Types with data: 0x10, 0x20, 0x21 (b1 b2 = data length, max 0x2006). Header-only: 0x90 (bumps a counter), 0xBB.
  - 0xBB from the host shows "Server Stop"/"Server Exited". The sign's own `A5 BB 00 00` every 20 s (0x4E20 ms) is its
    download server giving up after a quiet period.
  - 0x10 handling mentions "Bad CRC" / "Bad File CRC" (file transfer). The first data byte looks like a sub-command (limit 0x18).
  - The earlier scans all used sync A5, so the parser dropped every frame at the first byte.
  - It is an .lfs file download server: 0x20 start (data = [13]"X<8 hex of RAM word 0x98>.lfs" + size u32 + blocksize u16;
    a wrong name or length makes the sign reply `A5 20` with the name it expects), 0x21 blocks (u16 block no. + data;
    the last block also carries a u32 file CRC; replies 0x80 ack / 0x81 nak), 0x10 finish/verify ("Bad CRC"/"Bad File CRC"),
    0x90 bumps a counter, 0xBB stops the server. The heartbeat `A5 BB <status>`: 0 = OK, 1/2/3 = error codes.
  - `tools/signproto.py`: frame builder plus a model of the sign's parser (selftest passes; it regenerates the real heartbeat
    `a5bb0000583d`). Read-only probe: `signproto.py whoami` sends `5a 20 00 01 8b 77 00 00 00`
    and should get back `A5 20 ...` with the expected .lfs name. **Never send a correct 0x20/0x21/0x10 sequence: that writes flash.**

## Full decompilation (2026-10-30)
Ghidra 12.2 (`build/`), JDK 21 (JDK 27 crashes JPype), with the arm64 decompiler built from source. See
`analysis/ANALYSIS.md`. Outputs in `analysis/ghidra_out/`: `elite_rgb.c` (all 1522 functions, ~87k lines),
`bootstrap_full.c`, `comms.c`, `string_xref.txt`. A full pass after covering jump-table-only code gave `elite_rgb_full.c` (1916 fns, ~124k lines, 0 failures) and `bootstrap_full.c` (74 fns). The data section is decoded in `data_map.txt` (every region classified) and `data_section.md` (categorised: devices, files, config templates, protocols, UI, numeric tables).
- OS/CPU: RTEMS on MPC862 (PowerPC BE). Display `/dev/ledpanel` (content made for 80x16; the physical panel size is not confirmed).
- The stock firmware does not use Ethernet: it has no TCP/IP, socket, DHCP, TFTP or telnet strings. The PHY brings the
  link up alone, which is why the port is alive but silent. (Later: the network boot loader in `bootloader/` drives it.)
- DB9 `/dev/rs232` is an .lfs download server only (protocol fully mapped above).
- RS-485 `/dev/rs485` is the real content path (later: SCC2, and one of 12 selectable protocols; Cougar is the
  one the studio speaks): a slave on a jackpot bus, with a
  per-level Address and Text/Flash/Sound commands (menu handler FUN_000170bc, port opener FUN_00054404).
  The protocol is reversed in `analysis/PROTOCOL-RS485.md`. It is DLE-framed (STX 0x03 / ETX 0x04 / escape 0x10),
  CRC-16/CCITT reflected (poly 0x8408, init 0). Address 1 = jackpot/text broadcast of 69-byte records
  (flags: level nibble + 0x20 show; u32 value A; u32 value B; 60-byte text).
  `tools/signproto.py` builds these (`rs485_text_frame("...")`), verified against a model of the firmware parser.
  To drive it: a ~$10 USB-RS485 adapter on the RJ45 RS-485 port, plus the NVRAM config values (selected protocol,
  valid level ids, baud) read from the unit's menu. This path is send-only and updates the display; it does not write firmware flash.
