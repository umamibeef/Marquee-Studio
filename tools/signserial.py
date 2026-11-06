#!/usr/bin/env python3
"""Talk to the marquee sign's "RS232 DTE MODEM" port. Stdlib only (termios).

  signserial.py listen BAUD [SECONDS]   log everything the sign sends (hex + ascii)
  signserial.py scan [SECONDS]          listen at each common baud in turn
  signserial.py modem BAUD              pretend to be a Hayes modem, then log the session
  signserial.py blind SECONDS [BAUD...] answer every burst with OK/RING/CONNECT, undecoded
  signserial.py reply SECONDS [BODYHEX] at 115200, answer each sign frame with A5 BB <body> <CRC>

Output is verbose: port setup, every change on the handshake lines (CTS/DSR/DCD/RI),
a summary of each burst (bytes, duration, gap since the previous one) and every reply sent.
Logs go to ../captures/serial/<mode>-<baud>-<time>.log.
"""
import fcntl, glob, os, select, struct, sys, termios, time

BAUDS = [2400, 9600, 19200, 38400, 57600, 115200, 4800, 1200]
HERE = os.path.dirname(os.path.abspath(__file__))
BURST_GAP = 0.1  # seconds of silence that ends a burst

# macOS ioctls for the modem control lines
TIOCMGET, TIOCMBIS, TIOCMBIC = 0x4004746A, 0x8004746C, 0x8004746B
IOSSIOSPEED = 0x80085402
TIOCM_DTR, TIOCM_RTS = 0x002, 0x004
# Inputs on our side. Through the adapter's crossed pinout: CTS <- sign RTS, DSR <- sign DTR.
# DCD and RI are not driven by the sign (its pins 1 and 9 are inputs too).
IN_LINES = {"CTS (sign RTS)": 0x020, "DSR (sign DTR)": 0x100, "DCD": 0x040, "RI": 0x080}


def find_port():
    ports = sorted(glob.glob("/dev/cu.PL2303*") + glob.glob("/dev/cu.usbserial*"))
    if not ports:
        sys.exit("no USB-serial adapter found under /dev/cu.*")
    return ports[0]


def open_port(baud, log):
    port = find_port()
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = 0                                   # iflag: raw
    a[1] = 0                                   # oflag: raw
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    a[3] = 0                                   # lflag: raw
    std = getattr(termios, f"B{baud}", None)
    a[4] = a[5] = std if std is not None else termios.B9600
    termios.tcsetattr(fd, termios.TCSANOW, a)
    if std is None:  # non-standard speed: macOS needs IOSSIOSPEED
        fcntl.ioctl(fd, IOSSIOSPEED, struct.pack("L", baud))
    termios.tcflush(fd, termios.TCIOFLUSH)
    how = "standard" if std is not None else "non-standard, set via IOSSIOSPEED"
    log.info(f"opened {port} at {baud} baud ({how}), 8N1, no flow control, raw mode")
    return fd


def set_lines(fd, log, dtr=True, rts=True):
    for on, bit in ((dtr, TIOCM_DTR), (rts, TIOCM_RTS)):
        fcntl.ioctl(fd, TIOCMBIS if on else TIOCMBIC, struct.pack("I", bit))
    log.info(f"our DTR={'on' if dtr else 'off'} (-> sign DSR), "
             f"RTS={'on' if rts else 'off'} (-> sign CTS)")


def get_lines(fd):
    return struct.unpack("I", fcntl.ioctl(fd, TIOCMGET, struct.pack("I", 0)))[0]


class Log:
    def __init__(self, mode, baud):
        os.makedirs(os.path.join(HERE, "..", "captures", "serial"), exist_ok=True)
        name = f"{mode}-{baud}-{time.strftime('%H%M%S')}.log"
        self.path = os.path.join(HERE, "..", "captures", "serial", name)
        self.f = open(self.path, "w")
        self.t0 = time.time()
        self.total = 0

    def data(self, direction, b):
        self.total += len(b) if direction == "<" else 0
        for i in range(0, len(b), 16):
            chunk = b[i:i + 16]
            hx = " ".join(f"{c:02x}" for c in chunk)
            asc = "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)
            self.line(f"{direction} {hx:<47}  {asc}")

    def info(self, s):
        self.line(f"  [info] {s}")

    def line(self, s):
        s = f"{time.time() - self.t0:8.3f} {s}"
        print(s, flush=True)
        self.f.write(s + "\n")
        self.f.flush()


class Watch:
    """Tracks handshake-line changes and groups received bytes into bursts."""

    def __init__(self, fd, log):
        self.fd, self.log = fd, log
        self.lines = get_lines(fd)
        self.burst, self.burst_start, self.last_rx = b"", None, None
        self.last_burst_start, self.bursts = None, 0
        state = ", ".join(f"{n}={'on' if self.lines & m else 'off'}" for n, m in IN_LINES.items())
        log.info(f"input lines at start: {state}")

    def poll(self):
        """Call often. Returns the finished burst (bytes) once the line goes quiet, else None."""
        now_lines = get_lines(self.fd)
        for name, mask in IN_LINES.items():
            if (now_lines ^ self.lines) & mask:
                self.log.info(f"{name} changed -> {'on' if now_lines & mask else 'off'}")
        self.lines = now_lines
        if self.burst and time.time() - self.last_rx > BURST_GAP:
            return self._end_burst()
        return None

    def rx(self, b):
        now = time.time()
        if not self.burst:
            self.burst_start = now
        self.burst += b
        self.last_rx = now
        self.log.data("<", b)

    def _end_burst(self):
        b, self.burst = self.burst, b""
        self.bursts += 1
        dur = (self.last_rx - self.burst_start) * 1000
        gap = (f"{self.burst_start - self.last_burst_start:.3f}s after the previous burst"
               if self.last_burst_start else "first burst")
        self.last_burst_start = self.burst_start
        self.log.info(f"burst #{self.bursts}: {len(b)} bytes over {dur:.1f} ms, {gap}: {b.hex(' ')}")
        return b


def read(fd, timeout):
    r, _, _ = select.select([fd], [], [], timeout)
    if not r:
        return b""
    try:
        return os.read(fd, 4096)
    except BlockingIOError:
        return b""


def listen(baud, seconds):
    log = Log("listen", baud)
    log.info(f"mode: listen only, {seconds:.0f}s, log -> {log.path}")
    fd = open_port(baud, log)
    set_lines(fd, log)
    w = Watch(fd, log)
    end = time.time() + seconds
    try:
        while time.time() < end:
            b = read(fd, 0.05)
            if b:
                w.rx(b)
            w.poll()
    except KeyboardInterrupt:
        log.info("interrupted")
    w.poll()
    log.info(f"done at {baud} baud: {log.total} bytes in {w.bursts} bursts")
    os.close(fd)
    return log.total


def modem(baud):
    log = Log("modem", baud)
    log.info(f"mode: Hayes fake modem, log -> {log.path}")
    log.info("Ctrl-C to stop. Type 'r' + Enter to send RING, 'h' + Enter to hang up.")
    fd = open_port(baud, log)
    set_lines(fd, log)
    w = Watch(fd, log)

    def send(s, why):
        log.info(f"sending {s.strip().decode(errors='replace')!r}: {why}")
        os.write(fd, s)
        log.data(">", s)

    buf, connected, echo = b"", False, True
    log.info("[state] command mode, echo on")
    try:
        while True:
            r, _, _ = select.select([fd, sys.stdin], [], [], 0.05)
            w.poll()
            if sys.stdin in r:
                cmd = sys.stdin.readline().strip()
                if cmd == "r":
                    send(b"\r\nRING\r\n", "you asked for an incoming call")
                elif cmd == "h":
                    log.info("hanging up: dropping DTR for 0.5s")
                    connected = False
                    set_lines(fd, log, dtr=False)
                    time.sleep(0.5)
                    set_lines(fd, log)
                    send(b"\r\nNO CARRIER\r\n", "call ended")
                    log.info("[state] command mode")
            if fd not in r:
                continue
            b = read(fd, 0)
            if not b:
                continue
            w.rx(b)
            if connected:
                if b"+++" in b:
                    log.info("saw +++ escape, waiting 1s guard time")
                    time.sleep(1)
                    connected = False
                    send(b"\r\nOK\r\n", "escape to command mode")
                    log.info("[state] command mode")
                continue
            if echo:
                os.write(fd, b)
            buf += b
            while b"\r" in buf:
                line, buf = buf.split(b"\r", 1)
                cmd = line.strip().upper()
                if not cmd.startswith(b"AT"):
                    log.info(f"ignoring non-AT line: {line!r}")
                    continue
                log.info(f"AT command: {cmd.decode(errors='replace')}")
                if b"E0" in cmd:
                    echo = False
                    log.info("echo off")
                elif b"E1" in cmd:
                    echo = True
                    log.info("echo on")
                if cmd.startswith(b"ATD") or cmd.startswith(b"ATA"):
                    time.sleep(1)
                    send(f"\r\nCONNECT {baud}\r\n".encode(), "dial/answer request")
                    connected = True
                    log.info("[state] CONNECTED: everything below is the sign's own protocol")
                elif cmd.startswith(b"ATH"):
                    send(b"\r\nOK\r\n", "hang-up request")
                    connected = False
                else:
                    send(b"\r\nOK\r\n", "accept any other AT command")
    except KeyboardInterrupt:
        log.info("interrupted")
    log.info(f"stopped: {log.total} bytes in {w.bursts} bursts")
    os.close(fd)


def blind(baud, seconds):
    """Answer every burst with modem replies without decoding it: OK, then RING, then CONNECT."""
    log = Log("blind", baud)
    log.info(f"mode: blind fake modem, {seconds:.0f}s, log -> {log.path}")
    log.info("plan: 1st burst -> OK, +5s -> RING, every later burst -> CONNECT")
    fd = open_port(baud, log)
    set_lines(fd, log)
    w = Watch(fd, log)

    def send(s, why):
        log.info(f"sending {s.strip().decode()!r}: {why}")
        os.write(fd, s)
        log.data(">", s)

    end, ring_at = time.time() + seconds, None
    try:
        while time.time() < end:
            b = read(fd, 0.05)
            if b:
                w.rx(b)
            if w.poll() is not None:
                if w.bursts == 1:
                    send(b"\r\nOK\r\n", "reply to the first burst as if it were an AT command")
                    ring_at = time.time() + 5
                else:
                    send(f"\r\nCONNECT {baud}\r\n".encode(), f"reply to burst #{w.bursts}")
            if ring_at and time.time() >= ring_at:
                ring_at = None
                send(b"\r\nRING\r\n", "simulate an incoming call")
    except KeyboardInterrupt:
        log.info("interrupted")
    log.info(f"done at {baud} baud: {log.total} bytes in {w.bursts} bursts")
    os.close(fd)


def crc16_xmodem(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def frame(body):
    """Sign framing seen on the wire: A5 BB <body> <CRC-16/XMODEM of A5 BB + body, big-endian>."""
    d = b"\xa5\xbb" + body
    return d + crc16_xmodem(d).to_bytes(2, "big")


def reply(seconds, body_hex):
    """At 115200, answer each of the sign's frames with A5 BB <body_hex> + CRC."""
    baud = 115200
    log = Log("reply", baud)
    out = frame(bytes.fromhex(body_hex))
    log.info(f"mode: answer each sign frame with {out.hex(' ')}, {seconds:.0f}s, log -> {log.path}")
    fd = open_port(baud, log)
    set_lines(fd, log)
    w = Watch(fd, log)
    end = time.time() + seconds
    try:
        while time.time() < end:
            b = read(fd, 0.01)
            if b:
                w.rx(b)
            got = w.poll()
            if got is not None:
                if got[:2] == b"\xa5\xbb" and len(got) >= 6:
                    ok = crc16_xmodem(got[:-2]) == int.from_bytes(got[-2:], "big")
                    log.info(f"sign frame: body {got[2:-2].hex(' ') or '(empty)'}, CRC {'ok' if ok else 'BAD'}")
                log.info(f"sending reply {out.hex(' ')}")
                os.write(fd, out)
                log.data(">", out)
    except KeyboardInterrupt:
        log.info("interrupted")
    log.info(f"done: {log.total} bytes in {w.bursts} bursts")
    os.close(fd)


if __name__ == "__main__":
    args = sys.argv[1:]
    if not args or args[0] not in ("listen", "scan", "modem", "blind", "reply"):
        sys.exit(__doc__)
    if args[0] == "listen":
        listen(int(args[1]), float(args[2]) if len(args) > 2 else 3600)
    elif args[0] == "scan":
        secs = float(args[1]) if len(args) > 1 else 20
        for b in BAUDS:
            listen(b, secs)
    elif args[0] == "reply":
        reply(float(args[1]), args[2] if len(args) > 2 else "0000")
    elif args[0] == "blind":
        for b in args[2:] or BAUDS:
            blind(int(b), float(args[1]))
    else:
        modem(int(args[1]))
