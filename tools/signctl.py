#!/usr/bin/env python3
"""Drive the real sign through umami-marquee-sim's Sign window, so the person at the screen sees every step.

umami-marquee-sim (simulator/src/sign_ui.cpp) listens on build/sim/sign.sock. Each command runs in the Sign window exactly
as if its button had been pressed: the window comes to the front, its log, progress bar and console show the work,
and the answer here is the same text.

    tools/signctl.py status
    tools/signctl.py info                      # the boot loader's board info (restarts the sign), then boots it
    tools/signctl.py boots                     # boot history from the NVRAM debug record
    tools/signctl.py install [IMAGE]           # into the update slot (default: the window's image field)
    tools/signctl.py run [IMAGE] | uninstall | retry-slot | clear-crash | slot-stage | slot-direct
    tools/signctl.py console                   # connect the console pane
    tools/signctl.py send apps                 # a console line; waits for the prompt (8 s at most)
    tools/signctl.py send:30 sls               # ... waiting up to 30 s
    tools/signctl.py disconnect
    tools/signctl.py port /dev/cu.PL2303G-...  | image PATH | reset on|off

The chip write (stage 0) and the stage-1 loader stay buttons only.
"""
import os
import socket
import sys

SOCK = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "sim", "sign.sock")


def call(cmd, timeout=1800):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(SOCK)
    except OSError:
        raise SystemExit(f"umami-marquee-sim is not running (no {SOCK}): start it, e.g. build/simulator/umami-marquee-sim")
    s.sendall(cmd.encode() + b"\n")
    out = b""
    while True:
        b = s.recv(65536)
        if not b:
            break
        out += b
    return out.decode("latin-1")


if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print(__doc__)
        raise SystemExit(0)
    ans = call(" ".join(sys.argv[1:]))
    sys.stdout.write(ans)
    raise SystemExit(1 if ans.startswith("error:") or "Stopped:" in ans else 0)
