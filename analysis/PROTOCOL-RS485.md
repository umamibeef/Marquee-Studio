# RS-485 inbound protocol (the real content path)

Reverse-engineered from ELITE_RGB. The sign normally receives its text and jackpot values this way, as a slave
on an RS-485 bus. Frames on this bus change the display and jackpot state and never touch firmware flash, which
makes this the safe way to control the sign. It needs a USB-RS485 adapter (~$10) on `/dev/rs485` (SCC3/SCC4).

## Functions
- `FUN_00005728` lines 115-148: opens `/dev/rs485`, installs the stack, selects the protocol from
  `PTR_DAT_000b4e48[protocol_config]`, registers rx callback `FUN_00004b00`, spawns task "COUG" = `FUN_0002f7bc`.
- `FUN_0002f7bc`: rx byte state machine (DLE framing + CRC).
- `FUN_0002f544`: message dispatch (address + record decode).
- `FUN_0000b5b0` / table `0xb3a8c`: CRC-16/CCITT reflected, poly 0x8408, init 0x0000.
- Supported protocol names (selectable in the menu): JNMS Simple, IGT CCOM, Legacy-T, Mikohn CON2,
  Casino Link, TGS-NCP Links, Turbo, Cougar, Bally DIU, WMS. "Jupiters" is the site/customer config group.
  The frame format below is the one the running rx task (`FUN_0002f7bc`, task "COUG") decodes.

## Wire frame
```
0x03  <len_hi> <len_lo>  <addr_hi> <addr_lo>  <ctl>  <payload...>  <crc_hi> <crc_lo>  0x04
```
- `len = len(payload) + 5`  (addr 2 + ctl 1 + crc 2). The receiver reads `ctl` but ignores it; send 0x00.
  Earlier versions of this page and of `tools/signproto.py` left out the `ctl` byte, so frames built from them
  were misaligned and rejected.
- `crc` = CRC-16/CCITT reflected (poly 0x8408, init 0) over `len_hi..last payload byte` (including `ctl`), sent big-endian. A CRC field of `FFFF` is accepted unchecked.
- Byte stuffing: inside `len/addr/payload/crc`, any byte equal to 0x03, 0x04 or 0x10 is sent as
  `0x10, byte+0x10`; the receiver subtracts 0x10. A raw 0x03 restarts a frame; a raw 0x04 ends it.

## Address 1 = jackpot/text broadcast
`FUN_0002f544` handles `dest_addr == 1`: `payload = N × 69-byte records`.
```
record (69 bytes):
  +0x00  u8    flags   low nibble = level id (must match a configured level); 0x20 = show text;
                       0x40 = formatted/extra field; 0x80 = machine field
  +0x01  u32   value A (big-endian; e.g. jackpot amount)
  +0x05  u32   value B
  +0x09  char[60]  text, NUL-padded  -> rendered on the 80x16 panel
```
Other branches handle the other destination addresses (targeted/config). Address 1 is the broadcast that
drives the display, so it is the one to use first.

## Unknowns to resolve on the unit (NVRAM config, not in the image)
- The selected protocol (`Configure Protocol` menu). It must be the one matching these frames.
- The 4 valid level ids (`Jupiters,Level,%d,Address`). A record's level nibble must match one, or the record is ignored.
  Try level 0 first.
- The line baud rate for RS-485 (SCC3/SCC4). Check the menu or measure it with a logic analyzer on the RS-485 pair.

## Builder
`tools/signproto.py`:
- `rs485_frame(dest_addr, payload, ctl=0)`: ctl byte, stuffing + CRC.
- `coug_receive(stream)`: exact Python copy of the firmware receiver, used to test frames.
- `rs485_text_record(text, level=0, show=True, value_a=0, value_b=0)`: one 69-byte record.
- `rs485_text_frame(text, level=0)`: a ready frame to show `text` (addr 1, one record).
- `crc16_ccitt_refl()`: the CRC.

The builder is verified against a model of the firmware rx state machine: a built frame decodes back with CRC OK and the right
text, including when values contain the stuffed bytes. It is not yet tested on hardware, which needs the adapter and the NVRAM
config values above.

## Cougar receiver details (from `proto_coug_task`, 0x2F7BC)
- Frame: `03`, then `len_hi len_lo`, `addr_hi addr_lo`, one byte the receiver ignores, the payload,
  `crc_hi crc_lo`, `04`.
  - The payload length is (length field - 5).
  - The CRC is CRC-16/CCITT reflected (0x8408), computed over every byte from `len_hi` to the end of the
    payload, after unstuffing.
- Escaping: `0x10` (DLE) means the next byte has 0x10 subtracted. That byte may be 0x10..0x20, which
  encodes 0x00..0x10. A stray `03` restarts the frame; a stray `04` restarts it and counts an error.
- CRC bypass: a CRC field of `FFFF` is accepted without checking, which is useful for hand-made test frames.
- Address 1 carries N x 69-byte `jp_record`s:
  - `flags` (u8): the low nibble is the level number, matched against `cfg_level_address[4]`; 0x20 = show
    text; 0x40 = the text holds a machine number to look up in the 64-entry machine table; 0x80 = skip that lookup.
  - `value_a` (u32), `value_b` (u32), then 60 bytes of text.
  - A record is stored only when its level matches and flag 0x20 is set.
- Counters: good frames `coug_frames_ok`, bad CRC/ETX `coug_frame_errors`, bytes `coug_bytes_rx`.

## Frame checks of the other protocols

All three CRCs were checked by running the firmware's own routines in Unicorn on "123456789":

| protocol | CRC | where |
|---|---|---|
| Cougar, JWAM (receive) | the same reflected CCITT CRC one byte at a time (`crc16_ccitt_refl`, 0xB5B0); tools/signproto.py matches it on random frames | `proto_coug_task`, `proto_jwam_task` |
| JWAM replies | CRC-16/KERMIT (reflected CCITT, poly 0x8408, init 0; check 0x2189) over the length field onward | `jwam_reply_8003/8004/8005/8006` |
| GASS | CRC-16/MODBUS (poly 0xA001, init 0xFFFF; check 0x4B37) over every byte after the first, low byte first, frame ends in 0xFF | `gass_frame_finish`, `gass_ack_finish` |

JWAM reply layout: `03 <level address> <len16> <0x8000|cmd16> [payload] <crc16> 04`. The sign answers commands
3..6 with 0x8003..0x8006; 0x8004 and 0x8005 carry one status byte.

JWAM byte stuffing (`jwam_escape`, 0x317F0, applied by `proto_jwam_send` to every reply): between the first 2 bytes
and the last byte, each byte below 0x11 is sent as two bytes, `0x10, byte + 0x10` (so 0x10 itself goes out as `10 20`).
A receiver undoes it before the CRC check.

CCOM opens its port at 4800 baud for CCOM subtype 3, else 9600 (`ccom_port_setup`).

Mikohn CON2 (`mkhn_dispatch`, 0x396B4): V4.20 acts on only two commands. 0xF6 sets the jackpot values of the
matching levels; 0xF7 shows a message (blank text ends it). 0xEA, 0xEC, 0xF0, 0xF1, 0xF4, 0xF8 and every 0xF9/0xFA
sub-command are dispatched to empty functions (a single `blr`), so they are accepted and ignored.
