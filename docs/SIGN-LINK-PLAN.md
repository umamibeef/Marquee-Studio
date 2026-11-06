# The sign link and a serial debugger: plan

Goal: the box that runs Marquee Portal (a Raspberry Pi CM4 in an industrial case) sits inside the marquee's
enclosure and drives the sign over RS-485. The box's Ethernet is brought out to the back of the marquee; that is the
only network port. With our firmware, the sign also answers on RS-485 and accepts validated playlists and configs,
and its DB9 port gives the Pi a serial console and a GDB stub.

Nothing in this plan writes the main flash. The stock behaviour stays as it is whenever the new frames are not in
use.

## Why RS-485 and not the sign's Ethernet

An earlier version of this plan put lwIP and a signed UDP link into the firmware. With the Pi inside the case:
- the sign needs no network stack, no crypto and no boot-flash space for either;
- the stock firmware works from day one, because the sign already takes its content over RS-485
  (`sim/compare_rs485.py`); our firmware is an upgrade, not a requirement;
- the link to the sign never leaves the enclosure, and the exposed port is the Pi's, which runs a maintained Linux
  with HTTPS, a firewall and updates.

The sign's own Ethernet port stays unused. The network work done so far (the lwIP boot loader, `bootloader/net/`,
docs/NETWORK-STACKS.md) remains, as an option for later.

## Hardware

- **The box:** a Seeed reComputer R1125-10 inside the enclosure: a Raspberry Pi CM4 (4 GB RAM, 32 GB eMMC,
  Wi-Fi and BLE), two Ethernet ports, and two isolated RS-485 and two isolated RS-232 ports on terminal blocks
  (wiki.seeedstudio.com, reComputer R1100). One RS-485 port goes to the sign's bus, one RS-232 port to the sign's
  DB9 (an ADM211 RS-232 transceiver on the sign's board, docs/FIRMWARE-EXTRACTION.md), through a DB9-to-wire
  cable. The box's power comes from the enclosure.
- **Power loss:** Seeed's SuperCAP UPS LTC3350 module. It holds the CM4 up after the supply goes, signals the
  CPU, and a script shuts down. Seeed measured about 15 s idle and 6 s at full load (on its CM5 box); the shutdown
  script starts at once and stops Postgres first.
- **Ethernet:** a panel-mount RJ45 in the back of the marquee, with a short patch cable to the box.
  Wi-Fi (the hotspot) works from inside the case if the enclosure does not block it; to be checked.
- **To check:** the R1100 manual's terminal pinout and its RS-485 direction control and termination; on the sign
  (docs/BACKLOG.md, hardware queue), where the RS-485 and DB9 connectors are inside the case, a mounting place for
  the box away from the LED drivers' heat, and the Wi-Fi range through the enclosure.

## Bus speed

The sign's RS-485 speed is a menu setting; the studio's default is 19,200 baud, about 1.9 KB/s. That is ample for
text records, message lines and configs. A full rendered RGB555 frame (2.5 KB at 80 x 16) would take more than a
second, so the portal sends text and markup, not frames, and snapshots are an occasional, slow read. Which higher
rates the menu offers, and whether the sign's RS-485 transceiver handles them, is to be read from the firmware.

## Design

### 1. The stock path

The portal's `sender` drives the sign exactly as the studio does today: RS-485 records and marked-up message lines
(docs/MESSAGE-MARKUP.md), through the box's RS-485 port. Send-only: the stock firmware does not answer. This is
the Portal plan's `rs485` profile, now the main path.

### 2. Sign-link frames (our firmware)

Our firmware takes over `serial_read` for `/dev/rs485` and looks for sign-link frames in the byte stream; every
other byte goes to the active protocol task unchanged, so controllers and the portal's stock-path records work as
before.
- **Frame:** a start sequence that none of the sign's protocols (Mikohn, Cougar, TGSNCP) can produce, then `type`,
  `seq`, `length`, payload and a CRC-32. At most 512 bytes, so a frame takes a fraction of a second.
- **Types from the Pi:** `message` (a message line in the sign's markup, shown through the firmware's own message
  path), `config` (settings to change), `status` and `snapshot` (read requests), `ping`.
- **Replies from the sign:** an `ack` or `error` (with a reason code) for every frame, and the answers to read
  requests: firmware version, panel size, what is on the panel, protocol counters, the playlist and config
  versions it holds, a frame snapshot in chunks. The sign transmits only in answer to a frame addressed to it, so
  the bus stays half-duplex with the Pi as the only master. How the sign switches its RS-485 driver to transmit is
  to be found in the firmware's serial driver.
- **Larger documents** (a playlist of several message lines) go as numbered chunks, each acknowledged, and are
  applied only when complete.

### 3. Validation

Nothing is applied until it passes, in this order:
1. framing: the length is within the limit for its type, the CRC is correct, and a document's chunks add up;
2. version: newer than the one the sign holds (kept in NVRAM), so an old document is not applied again;
3. content: every message line parses with the firmware's own markup parser and fits its limits; every config
   setting is a known setting, of the right type, within the range the firmware's own menu allows for it.

A frame or document that fails changes nothing and gets an `error` with the reason. A config is applied all or
nothing: every setting is checked first, then they are written together to NVRAM, the same way the menu saves an
edit, and the reply holds the values the sign now has.

The bus is inside the enclosure, so frames are not signed by default. An HMAC-SHA256 with a pairing key can be
switched on; at bus speed it costs the sign nothing noticeable.

What a config can change: the settings the menu can change (the menu's table gives names, types and ranges).
Nothing reaches flash: there is no frame type for a flash write, a code load or a memory write.

### 4. Playlist cache

The sign keeps the last good playlist in RAM and shows it if the Pi stops sending (for instance while the box
restarts).

### 5. Getting the code into the firmware

The extension mechanism (`analysis/tools/fwext.py`, docs/NATIVE-EMOJI.md) stores extension code in the image's
free gaps, about 8 KB in all, of which native emoji uses 3.8 KB. The frame parser, validation and the console are
estimated at 6 to 10 KB, so they may not fit. If not, a second-stage loader: a small hook in the gaps loads a
compressed `link.ext` from the boot flash's LFS after the volumes mount, into a larger reserved RAM block, and calls
its init. If the file is missing or bad, nothing starts and the firmware runs as stock.

## The serial debugger (DB9, from the Pi)

The DB9 port (`/dev/rs232`, SMC1, 115200 8N1) is today the download server's: the `SMOD` task reads it, and only a
valid transfer sequence makes it write flash. It is wired to one of the box's RS-232 ports, so the console and the GDB stub
are reachable from the Pi, over SSH from the back of the marquee.

- **Console.** `serial_read` for `/dev/rs232` is taken over, as for RS-485. A magic line opens a text console;
  until then every byte goes to the download server unchanged, so the DB9 still works as before. Commands: version
  and build, tasks (from the RTEMS thread table), heap, memory peek, NVRAM dump (read-only), sign-link counters,
  log tail, frame snapshot (at 115200, faster than over RS-485), panel test patterns, restart. Leaving the console
  returns the port to the download server. No command writes flash.
- **Log.** A ring buffer in the extension's RAM block that the extensions (and later wrapped firmware functions)
  write to; readable from the console and with a sign-link `status`.
- **GDB stub.** A console command switches the port to the GDB remote serial protocol:
  - registers and memory read/write (RAM only; flash is refused);
  - software breakpoints by writing `tw` trap instructions into the code (it runs from RAM) with cache sync;
  - single step through the trace exception (MSR[SE], vector 0xD00);
  - break-in with Ctrl-C (checked by the serial interrupt).
  The program (0x700) and trace (0xD00) vectors are taken over the same way as the text routines; while stopped,
  interrupts are off and RTEMS is frozen. Thread awareness (listing RTEMS tasks to GDB) comes after.
  From a desktop: `ssh` to the Pi with a port forward, or `ser2net` on the Pi bound to localhost.
- **Who gets it.** The console and the GDB stub are in development builds (`fwext.py --debug`). A release build
  keeps only the read-only console commands, behind the magic line.
- **Never on the Pi's side:** a valid DB9 transfer sequence. The Pi's tools for the DB9 do not implement it.

## Milestones

| # | Milestone | Where | Done when |
|---|---|---|---|
| 0 | The box in the sign | the sign | The box and its UPS module are mounted and powered from the enclosure; RS-485 and DB9 are wired; the rear RJ45 works; the first live RS-485 send of a marked-up record shows on the panel (BACKLOG item 4) |
| 1 | The stock path from the Pi | Pi + sign | The portal's `sender` drives the sign through the box's RS-485 port, with stock firmware; pulling the power shuts the box down cleanly |
| 2 | Serial console | simulator | The console opens with the magic line, every command answers, and the download server still answers its read-only probes the same (`compare_db9.py`) |
| 3 | Sign-link frames | simulator | Frames are recognised and answered; with frames interleaved, every protocol's records are handled as on the stock firmware (`compare_rs485.py`); corrupt, oversized and truncated frames change nothing and get the right `error` |
| 4 | Messages and configs | simulator | A `message` shows as the same line does in a message slot; a `config` leaves the same NVRAM as the same edit made in the menu (`compare_menu.py`); an out-of-range setting is refused and nothing is written; a playlist sent in chunks survives lost and repeated chunks |
| 5 | GDB stub | simulator | A Python RSP client reads registers and memory, sets and hits a breakpoint, single-steps, continues, and breaks in with Ctrl-C |
| 6 | The portal on the link | Pi | The `sender` uses sign-link frames when the sign answers `ping`, and the stock path otherwise; the portal shows the sign's status |
| 7 | On the sign | the sign | Milestones 2 to 6 on the hardware, on a spare boot-flash chip |

Milestones 2 to 5 are built and tested in the simulator and do not wait for the sign.

## Verification

- Every milestone keeps the existing comparisons green: with no sign-link frames and the console closed, the
  firmware matches the stock firmware in `compare.py`, `compare_rs485.py`, `compare_db9.py` and `compare_menu.py`.
- The simulator's UART model gets a transmit side for RS-485 replies, and a Python peer that plays the Pi.
- Fuzzing: the frame parser and the validation are fed random and mutated frames in the simulator.
- The display's frame rate is measured with the link busy.

## Risks

- **Frame start sequence.** It must never occur in any protocol's traffic; it is chosen from the three parsers'
  grammars and tested with each protocol's recorded traffic in the simulator.
- **RS-485 transmit on the sign.** The stock firmware may never drive the bus; the driver-enable line has to be
  found in the firmware and checked on the board.
- **Space** for the extension code (design 5).
- **Exception vectors** for the GDB stub are hand-written assembly in the original; taking them over needs care
  and is tested in the simulator first.

## Questions for the owner

1. Should the release build keep the read-only serial console, or only development builds?
2. Is the sign's menu worth extending (link status, pairing), or are the portal and the console enough?
