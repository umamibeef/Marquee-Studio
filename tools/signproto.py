#!/usr/bin/env python3
"""Read-only probes of the sign's DB9 download protocol. The protocol itself (frames, CRCs, RS-485 records) is in
marquee_pack/signproto.py; this tool re-exports it, so `import signproto` keeps working.

  signproto.py whoami        send a deliberately invalid 0x20 header; the sign answers with the file name it expects
  signproto.py listen SECS   decode the sign's frames for SECS seconds
  signproto.py selftest      check the frame builder against a model of the sign's receive parser (no hardware)
Only read-only probes are offered here. Nothing in this tool starts a transfer or writes to the sign.
"""
import os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
from marquee_pack.signproto import *  # noqa: E402,F401,F403
from marquee_pack.signproto import _stuff  # noqa: E402,F401


def run(mode, seconds=25.0):
    import signserial as s
    log = s.Log(f"proto-{mode}", 115200)
    fd = s.open_port(115200, log)
    s.set_lines(fd, log)
    if mode == "whoami":
        out = frame(0x20, b"\x00")   # invalid on purpose: length 1, name check fails -> sign replies with its name
        log.info(f"sending invalid start-transfer header (read-only probe): {out.hex(' ')}")
        os.write(fd, out)
        log.data(">", out)
    buf, end = b"", time.time() + seconds
    while time.time() < end:
        b = s.read(fd, 0.05)
        if b:
            log.data("<", b)
            frames, buf = parse_sign(buf + b)
            for f in frames:
                log.info("frame: " + describe(f))
    os.close(fd)



if __name__ == "__main__":
    a = sys.argv[1:]
    if not a or a[0] not in ("whoami", "listen", "selftest"):
        sys.exit(__doc__)
    if a[0] == "selftest":
        selftest()
    else:
        run(a[0], float(a[1]) if len(a) > 1 else 25.0)
