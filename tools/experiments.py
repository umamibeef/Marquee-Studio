#!/usr/bin/env python3
"""Low-risk RS-232 experiments on the sign's DB9 at 115200. Logs go to ../captures/serial/.

  experiments.py bootspam [SECONDS]  send interrupt keys every 50 ms and BREAK every 2 s;
                                     power the sign on while this runs (default 60 s)
  experiments.py ack                 answer heartbeats within ~1 ms with candidate replies,
                                     one per heartbeat, and watch whether the rhythm changes
  experiments.py lines               drop our DTR/RTS (sign DSR/CTS), then toggle them,
                                     watching the heartbeat
"""
import os, sys, termios, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import signserial as s  # noqa: E402

HEARTBEAT = bytes.fromhex("a5bb0000583d")
BAUD = 115200


def setup(mode):
    log = s.Log(mode, BAUD)
    fd = s.open_port(BAUD, log)
    s.set_lines(fd, log)
    return log, fd, s.Watch(fd, log)


def bootspam(seconds):
    log, fd, w = setup("bootspam")
    keys = [b"\r", b" ", b"\x03", b"\x1b", b"*", b"#", b"?", b"\n"]
    log.info(f"sending {', '.join(repr(k) for k in keys)} in turn every 50 ms, BREAK every 2 s, "
             f"for {seconds:g} s. Power the sign on now.")
    end, i, next_break = time.time() + seconds, 0, time.time() + 2
    try:
        while time.time() < end:
            os.write(fd, keys[i % len(keys)])
            i += 1
            if time.time() >= next_break:
                log.info("sending BREAK")
                termios.tcsendbreak(fd, 0)
                next_break = time.time() + 2
            b = s.read(fd, 0.05)
            if b:
                w.rx(b)
            got = w.poll()
            if got is not None and got != HEARTBEAT:
                log.info("*** something other than the heartbeat came back ***")
    except KeyboardInterrupt:
        log.info("interrupted")
    log.info(f"done: sent {i} keys, received {log.total} bytes in {w.bursts} bursts")
    os.close(fd)


def ack():
    log, fd, w = setup("ack")
    f = s.frame
    cands = [
        ("echo of the heartbeat", HEARTBEAT),
        ("swapped sync BB A5", b"\xbb\xa5\x00\x00" + s.crc16_xmodem(b"\xbb\xa5\x00\x00").to_bytes(2, "big")),
        ("body 00 01", f(b"\x00\x01")),
        ("body 01 00", f(b"\x01\x00")),
        ("body 80 00", f(b"\x80\x00")),
        ("body 00 80", f(b"\x00\x80")),
        ("body 06 00 (ACK)", f(b"\x06\x00")),
        ("body 00 06", f(b"\x00\x06")),
        ("bare ASCII ACK 0x06", b"\x06"),
        ("heartbeat with little-endian CRC", bytes.fromhex("a5bb00003d58")),
    ]
    log.info(f"{len(cands)} candidates, one per heartbeat (~{len(cands) * 20 / 60:.1f} min). "
             "A normal heartbeat comes 20.0 s after the line was last active.")
    buf, idx, last_sent = b"", 0, None
    try:
        while idx < len(cands) or (last_sent and time.time() - last_sent < 25):
            b = s.read(fd, 0.001)
            if b:
                w.rx(b)
                buf = (buf + b)[-64:]
            if HEARTBEAT in buf:
                now = time.time()
                if last_sent:
                    log.info(f"heartbeat {now - last_sent:.3f} s after our last reply "
                             f"({'normal' if 19.5 < now - last_sent < 20.5 else '*** DIFFERENT ***'})")
                buf = b""
                if idx == len(cands):
                    break  # the heartbeat after the last candidate has been checked
                if idx < len(cands):
                    name, out = cands[idx]
                    os.write(fd, out)
                    last_sent = time.time()
                    log.info(f"replied with {name}: {out.hex(' ')}")
                    log.data(">", out)
                    idx += 1
            elif buf and buf[:1] != HEARTBEAT[:1]:
                log.info(f"*** unexpected bytes: {buf.hex(' ')} ***")
                buf = b""
            w.poll()
            if last_sent and time.time() - last_sent > 25:
                log.info("*** no heartbeat within 25 s of our reply: the sign changed behavior ***")
                last_sent = None
    except KeyboardInterrupt:
        log.info("interrupted")
    log.info(f"done: {log.total} bytes received in {w.bursts} bursts")
    os.close(fd)


def lines():
    log, fd, w = setup("lines")
    steps = [("DTR off, RTS off", False, False, 45), ("DTR on, RTS off", True, False, 25),
             ("DTR off, RTS on", False, True, 25), ("DTR on, RTS on", True, True, 25)]
    try:
        for label, dtr, rts, secs in steps:
            log.info(f"--- {label} for {secs} s ---")
            s.set_lines(fd, log, dtr=dtr, rts=rts)
            end = time.time() + secs
            while time.time() < end:
                b = s.read(fd, 0.05)
                if b:
                    w.rx(b)
                w.poll()
        log.info("--- toggling DTR every 0.5 s for 10 s ---")
        for i in range(20):
            s.set_lines(fd, log, dtr=bool(i % 2), rts=True)
            end = time.time() + 0.5
            while time.time() < end:
                b = s.read(fd, 0.05)
                if b:
                    w.rx(b)
                w.poll()
        s.set_lines(fd, log)
        end = time.time() + 25
        while time.time() < end:
            b = s.read(fd, 0.05)
            if b:
                w.rx(b)
            w.poll()
    except KeyboardInterrupt:
        log.info("interrupted")
    log.info(f"done: {log.total} bytes in {w.bursts} bursts")
    os.close(fd)


if __name__ == "__main__":
    a = sys.argv[1:]
    if not a or a[0] not in ("bootspam", "ack", "lines"):
        sys.exit(__doc__)
    if a[0] == "bootspam":
        bootspam(float(a[1]) if len(a) > 1 else 60)
    elif a[0] == "ack":
        ack()
    else:
        lines()
