"""Mikohn/Neurizon sign download protocol on the DB9 (115200 8N1), reconstructed from ELITE_RGB.

Host -> sign:  5A <type> <b1> <b2> <hdrCRC hi lo> [data (b1b2 bytes) <dataCRC hi lo>]
Sign -> host:  A5 <type> <b1> <b2> <hdrCRC hi lo> [data <dataCRC>]
  hdrCRC = CRC-16/XMODEM over sync+type+b1+b2; dataCRC = CRC-16/XMODEM over the data.
  Types with data (b1b2 = length): 0x10 finish/verify, 0x20 start transfer, 0x21 data block.
  Header-only (b1b2 = parameter): 0x90, 0xBB (server stop). Sign replies: 0x80 ack, 0x81 nak, 0x20 expected name,
  0xBB status (every 20 s of silence).

This module only builds and parses frames; it opens no port. The read-only probes (whoami, listen) are in
tools/signproto.py. Nothing here starts a transfer or writes to the sign.
"""

HOST_SYNC, SIGN_SYNC = 0x5A, 0xA5
DATA_TYPES, HEADER_TYPES = (0x10, 0x20, 0x21), (0x90, 0xBB)
NAMES = {0x10: "finish", 0x20: "start/name", 0x21: "block", 0x80: "ack", 0x81: "nak", 0x90: "0x90", 0xBB: "status/stop"}


def crc16(data, crc=0):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def frame(ftype, data=b"", param=0, sync=HOST_SYNC):
    """Build a frame. Data types carry `data`; header-only types carry the 16-bit `param`."""
    if ftype in DATA_TYPES:
        hdr = bytes([sync, ftype, len(data) >> 8, len(data) & 0xFF])
        return hdr + crc16(hdr).to_bytes(2, "big") + data + crc16(data).to_bytes(2, "big")
    hdr = bytes([sync, ftype, param >> 8, param & 0xFF])
    return hdr + crc16(hdr).to_bytes(2, "big")


def sign_parser_model(stream):
    """Python model of the sign's receive state machine (ELITE_RGB 0x33468). Returns accepted (type, b1b2, data)."""
    out, st, i = [], 1, 0
    while i < len(stream):
        b = stream[i]; i += 1
        if st == 1:
            if b == 0x5A:
                hcrc, st = crc16([0x5A]), 2
        elif st == 2:
            hcrc = crc16([b], hcrc)
            if b in DATA_TYPES:
                ftype, st = b, 3
            elif b in HEADER_TYPES:
                ftype, st = b, 5
            else:
                st = 1
        elif st in (3, 5):
            hcrc = crc16([b], hcrc); val = b << 8; st += 1
        elif st in (4, 6):
            hcrc = crc16([b], hcrc); val |= b
            st = 1 if (st == 4 and val > 0x2006) else 7
        elif st == 7:
            got = b << 8; st = 8
        elif st == 8:
            got |= b
            if got != hcrc:
                st = 1
            elif ftype in HEADER_TYPES:
                out.append((ftype, val, b"")); st = 1
            else:
                data, st = bytearray(), 9
        elif st == 9:
            data.append(b)
            if len(data) == val:
                st = 10
        elif st == 10:
            got = b << 8; st = 11
        elif st == 11:
            got |= b
            if got == crc16(data):
                out.append((ftype, val, bytes(data)))
            st = 1
    return out


def parse_sign(buf):
    """Pull complete A5 frames out of a byte buffer. Returns (frames, leftover)."""
    frames = []
    while True:
        i = buf.find(bytes([SIGN_SYNC]))
        if i < 0:
            return frames, b""
        buf = buf[i:]
        if len(buf) < 6:
            return frames, buf
        ftype, val = buf[1], (buf[2] << 8) | buf[3]
        ok = crc16(buf[:4]) == int.from_bytes(buf[4:6], "big")
        if not ok:
            buf = buf[1:]
            continue
        if ftype in DATA_TYPES:
            if len(buf) < 6 + val + 2:
                return frames, buf
            data = buf[6:6 + val]
            dok = crc16(data) == int.from_bytes(buf[6 + val:8 + val], "big")
            frames.append((ftype, val, data, dok)); buf = buf[8 + val:]
        else:
            frames.append((ftype, val, b"", True)); buf = buf[6:]


def describe(f):
    ftype, val, data, ok = f
    s = f"A5 {ftype:02X} ({NAMES.get(ftype, '?')}) {'len' if ftype in DATA_TYPES else 'param'}={val:#06x}"
    if data:
        s += f" data={data.hex(' ')}"
        if ftype == 0x20 and data[0] < len(data):
            s += f"  name={data[1:1 + data[0]].decode('latin-1')!r}"
    return s + ("" if ok else "  DATA CRC BAD")


def selftest():
    w = frame(0x20, b"\x00")
    print("whoami frame:", w.hex(" "))
    assert sign_parser_model(w) == [(0x20, 1, b"\x00")], sign_parser_model(w)
    assert sign_parser_model(frame(0x90)) == [(0x90, 0, b"")]
    assert sign_parser_model(frame(0x20, b"\x00", sync=0xA5)) == []          # our old mistake: A5 sync is ignored
    hb = frame(0xBB, sync=SIGN_SYNC)
    assert hb.hex() == "a5bb0000583d", hb.hex()                              # matches the real heartbeat
    reply = frame(0x20, b"\x0dX12345678.lfs", sync=SIGN_SYNC)
    print("parsed sample reply:", describe(parse_sign(reply)[0][0]))
    print("selftest OK")


# ---------------------------------------------------------------------------
# RS-485 inbound protocol (the real content path), reversed from ELITE_RGB:
#   rx state machine FUN_0002f7bc, dispatch FUN_0002f544, CRC FUN_0000b5b0 (table 0xb3a8c).
# Wire frame (DLE-stuffed):
#   0x03  <len_hi> <len_lo>  <addr_hi> <addr_lo>  <ctl>  <payload...>  <crc_hi> <crc_lo>  0x04
#   len   = len(payload) + 5               (addr 2 + ctl 1 + crc 2)
#   ctl   = one byte the receiver reads but ignores (only covered by the CRC); we send 0x00.
#           (Fixed 2026-10-31: earlier versions omitted it, so every frame was misaligned.)
#   crc   = CRC-16/CCITT reflected, poly 0x8408, init 0x0000, over len+addr+ctl+payload (big-endian
#           on wire). A CRC field of 0xFFFF is accepted without checking.
#   stuffing: a data byte 0x03/0x04/0x10 is sent as 0x10,(byte+0x10); decoder subtracts 0x10
#   addr 1 = jackpot/text broadcast: payload = N records of 69 bytes:
#     +0x00 flags  (low nibble = level id that must match a configured level; 0x20 = show text;
#                   0x40 = formatted/extra; 0x80 = machine field)
#     +0x01 u32    value A (e.g. jackpot amount)
#     +0x05 u32    value B
#     +0x09 char[60] text (NUL-padded)        -> shown on the 80x16 panel
# NOTE: needs a USB-RS485 adapter. This path drives the DISPLAY/NVRAM state, NOT firmware flash.
# The sign's address, protocol selection and the 4 valid level ids are NVRAM config (set on the unit);
# level 0 + flags 0x20 is the first thing to try. Send-only; untested on hardware.

RS485_STX, RS485_ETX, RS485_DLE = 0x03, 0x04, 0x10


def crc16_ccitt_refl(data, crc=0):
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return crc


def _stuff(data):
    out = bytearray()
    for b in data:
        if b in (RS485_STX, RS485_ETX, RS485_DLE):
            out += bytes([RS485_DLE, b + 0x10])
        else:
            out.append(b)
    return bytes(out)


def rs485_frame(dest_addr, payload, ctl=0x00):
    """Build one DLE-stuffed RS-485 frame for the sign."""
    length = len(payload) + 5
    body = bytes([length >> 8, length & 0xFF, dest_addr >> 8, dest_addr & 0xFF, ctl]) + payload
    crc = crc16_ccitt_refl(body)
    body += bytes([crc >> 8, crc & 0xFF])
    return bytes([RS485_STX]) + _stuff(body) + bytes([RS485_ETX])


def rs485_text_record(text, level=0, show=True, value_a=0, value_b=0):
    """One 69-byte jackpot/text record."""
    flags = (level & 0x0F) | (0x20 if show else 0)
    rec = bytes([flags]) + value_a.to_bytes(4, "big") + value_b.to_bytes(4, "big")
    rec += text.encode("latin-1", "replace")[:60].ljust(60, b"\x00")   # emoji not in the font -> '?'
    assert len(rec) == 69
    return rec


def coug_receive(stream):
    """Exact Python copy of the firmware receiver (proto_coug_task, 0x2F7BC), for testing frames.
    Returns a list of (dest_addr, payload) for every frame it would dispatch."""
    out, state, dle, crc, rx_crc = [], 0, 0, 0, 0
    plen = addr = idx = 0
    buf = bytearray()
    for b in stream:
        if b == 3:
            state, dle = 0, 0
        if b == 4 and state != 9:
            state, dle = 0, 0
        if not dle:
            if b == 0x10:
                dle = 1
        else:
            u = (b - 0x10) & 0xFF
            if u < 0x11:
                dle, b = 0, u
            else:
                state = 0
        if dle:
            continue
        if state == 0:
            if b == 3:
                state = 1
        elif state == 1:
            crc = crc16_ccitt_refl([b]); plen = b << 8; state = 2
        elif state == 2:
            crc = crc16_ccitt_refl([b], crc); plen = ((plen | b) - 5) & 0xFFFF; state = 3
        elif state == 3:
            crc = crc16_ccitt_refl([b], crc); addr = b << 8; state = 4
        elif state == 4:
            crc = crc16_ccitt_refl([b], crc); addr |= b; state = 5
        elif state == 5:
            crc = crc16_ccitt_refl([b], crc); idx = 0; buf = bytearray(); state = 6
        elif state == 6:
            crc = crc16_ccitt_refl([b], crc); buf.append(b); idx += 1
            if idx == plen:
                state = 7
        elif state == 7:
            rx_crc = b << 8; state = 8
        elif state == 8:
            rx_crc |= b; state = 9
        elif state == 9:
            if b == 4 and rx_crc in (crc, 0xFFFF):
                out.append((addr, bytes(buf)))
            state = 0
    return out


def rs485_text_frame(text, level=0, dest=1):
    """A ready frame that should show `text` on the panel (addr 1 broadcast, one record)."""
    return rs485_frame(dest, rs485_text_record(text, level=level))


def rs485_markup_record(data, level=0, show=True, value_a=0, value_b=0):
    """rs485_text_record for raw text bytes, e.g. a marked-up message line (studio/core.py encode_markup,
    docs/MESSAGE-MARKUP.md). The bytes go in unchanged, NUL-padded to 60; more than 60 is an error.
    Whether the sign runs record text through the markup tokenizer is not verified."""
    data = bytes(data)
    if len(data) > 60:
        raise ValueError(f"{len(data)} bytes: a jackpot/text record holds 60")
    flags = (level & 0x0F) | (0x20 if show else 0)
    rec = bytes([flags]) + value_a.to_bytes(4, "big") + value_b.to_bytes(4, "big") + data.ljust(60, b"\x00")
    assert len(rec) == 69
    return rec


def rs485_markup_frame(data, level=0, dest=1):
    """rs485_text_frame for raw text bytes (see rs485_markup_record)."""
    return rs485_frame(dest, rs485_markup_record(data, level=level))
