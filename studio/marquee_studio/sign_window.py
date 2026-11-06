"""The real sign (or the emulated one) through umami-marquee-sim's Sign window: every upload to a sign goes there, so
it shows in that window (simulator/src/sign_ui.cpp, its Content tab), never straight to the serial port from here.

An upload is a folder build/sim/uploads/<stamp>/: upload.txt ("port PATH", "file NAME", "cmd LINE" lines, in order)
and files/<NAME>. The Sign window replaces the sign's content store with the files (sbegin, sfile, sraw pieces,
scommit) and then runs the lines in the sign's console; its log is the answer."""
import os
import shutil
import socket
import time

from . import paths

SOCK = os.path.join(paths.REPO, "build", "sim", "sign.sock")
UPLOADS = os.path.join(paths.REPO, "build", "sim", "uploads")
# In a container the host's Unix socket can't be reached (Docker Desktop's VM); SIGN_WINDOW_TCP="host:port" points at
# the sim's TCP control (UMAMI_SIGN_TCP) instead, over host.docker.internal. The uploads folder is bind-mounted.
TCP = os.environ.get("SIGN_WINDOW_TCP")
KEEP = 8                                        # upload folders kept (the newest), for "Upload again"


class SignWindowError(RuntimeError):
    pass


def _connect(timeout):
    if TCP:
        host, _, port = TCP.rpartition(":")
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(timeout)
        s.connect((host, int(port)))
        return s
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(SOCK)
    return s


def available():
    try:
        _connect(2).close()
        return True
    except OSError:
        return False


def ports():
    """serial ports the Sign window (this host's sim) can open, for a Studio that can't see them itself (a container)"""
    out = call("ports", timeout=5)
    return [l for l in (ln.strip() for ln in out.splitlines()) if l and l != "(none)"]


def send_rs485(port, baud, frame):
    """one RS-485 frame through the Sign window (this host opens the adapter and writes it); the window's reply"""
    out = call(f"rs485 {port} {int(baud)} {bytes(frame).hex()}", timeout=30).strip()
    if out.startswith("error:"):
        raise SignWindowError(out)
    return out


def call(cmd, timeout=1800):
    """one command to the Sign window; its answer (the window's log for a job)"""
    try:
        s = _connect(timeout)
    except OSError:
        where = f"at {TCP}" if TCP else "(build/simulator/umami-marquee-sim)"
        raise SignWindowError("umami-marquee-sim is not running: start it " + where + "; "
                              "uploads to the sign go through its Sign window") from None
    try:
        s.sendall(cmd.encode() + b"\n")
        out = b""
        while True:
            b = s.recv(65536)
            if not b:
                break
            out += b
    finally:
        s.close()
    return out.decode("latin-1")


def _folder(port, files, cmds):
    os.makedirs(UPLOADS, exist_ok=True)
    old = sorted(os.listdir(UPLOADS))
    for d in old[:max(0, len(old) - KEEP + 1)]:
        shutil.rmtree(os.path.join(UPLOADS, d), ignore_errors=True)
    d = os.path.join(UPLOADS, time.strftime("%Y%m%d-%H%M%S") + f"-{time.time_ns() % 1000000:06d}")
    os.makedirs(os.path.join(d, "files"))
    lines = [f"port {port}"]
    for name, data in files.items():
        if "/" in name or "\n" in name:
            raise ValueError(f"bad file name {name!r}")
        with open(os.path.join(d, "files", name), "wb") as f:
            f.write(data)
        lines.append(f"file {name}")
    lines += [f"cmd {c}" for c in cmds]
    with open(os.path.join(d, "upload.txt"), "w", encoding="latin-1") as f:
        f.write("\n".join(lines) + "\n")
    return d


def upload(port, files, cmds, feed=False):
    """files ({name: bytes}) as the sign's new content store (none: the store is left alone), then the console lines
    cmds; through the Sign window. Returns {"log": [...], "replies": {cmd: [lines]}}; raises SignWindowError when the
    window stopped (its reason)."""
    d = _folder(port, files, cmds)
    ans = call(("feed " if feed else "upload ") + d)
    log = [l for l in ans.split("\n") if l]
    if ans.startswith("error:"):
        raise SignWindowError(ans.strip())
    stopped = [l for l in log if l.startswith("Stopped:")]
    if stopped:
        raise SignWindowError(stopped[-1][len("Stopped: "):])
    replies, cur = {}, None
    for l in log:
        if l.startswith("> "):
            cur = l[2:]
            replies[cur] = []
        elif l.startswith("  | ") and cur is not None:
            replies[cur].append(l[4:])
    return {"log": log, "replies": replies}
