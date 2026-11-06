#!/usr/bin/env python3
"""Run the NET=1 bootloader on the emulated board (test/cosim.py) with an emulated Ethernet:
an MPC862 FEC + LXT972A PHY model and a scripted host on the other end of the cable.

The host:
  1. watches the bootloader ask for DHCP (no server answers, so it must fall back to
     169.254.77.1 after 3 s of simulated time);
  2. ARP-resolves and pings it;
  3. downloads info.txt and bootflash.bin over TFTP (the latter must equal the flash);
  4. uploads the real ELITE_RGB archive entry over TFTP;
and the bootloader must then boot that upload from RAM: reach address 0 with the
application decompressed in SDRAM.

Usage: netsim.py NET_FLASH_IMAGE ORIGINAL_DUMP ELITE_RGB
"""
import struct, sys, time
sys.path.insert(0, __file__.rsplit("/", 1)[0])
import cosim
from cosim import be32, NVRAM

DEV_IP, HOST_IP = bytes([169, 254, 77, 1]), bytes([169, 254, 77, 2])
HOST_MAC, BCAST = bytes.fromhex("020000000002"), b"\xff" * 6
PHY_ADDR = 1
TICKS_PER_READ = 40          # TCN2 advance per read: 0.2 ms at 200 ticks/ms


def csum(b):
    if len(b) % 2:
        b += b"\0"
    s = sum(struct.unpack(f">{len(b) // 2}H", b))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def ipv4(src, dst, proto, payload):
    h = struct.pack(">BBHHHBBH4s4s", 0x45, 0, 20 + len(payload), 1, 0, 64, proto, 0, src, dst)
    return h[:10] + struct.pack(">H", csum(h)) + h[12:] + payload


def udp(src, dst, sport, dport, data):
    u = struct.pack(">HHHH", sport, dport, 8 + len(data), 0) + data
    pseudo = src + dst + struct.pack(">BBH", 0, 17, len(u))
    c = csum(pseudo + u) or 0xFFFF
    return ipv4(src, dst, 17, u[:6] + struct.pack(">H", c) + u[8:])


def eth(dst, src, etype, payload):
    return dst + src + struct.pack(">H", etype) + payload


class NetBoard(cosim.Board):
    """cosim.Board plus a FEC/PHY model and a free-running CPM timer 2."""

    def __init__(self, *a, **k):
        super().__init__(*a, **k)
        self.tcn2 = 0
        self.ievent = 0
        self.phy = {0: 0x1000, 1: 0x782D, 2: 0x0013, 3: 0x78E2, 4: 0x01E1, 5: 0x45E1, 17: 0x4E00}
        self.cable = True
        self.rx_idx = self.tx_idx = 0
        self.to_host, self.to_dev = [], []
        self.reached_app = False
        self.uc.reg_write(cosim.UC_PPC_REG_PC, cosim.FLASH + 0x100)   # reset vector

    def _rd(self, uc, off, size, _):
        if off == 0x99E:                                   # TCN2
            self.tcn2 = (self.tcn2 + TICKS_PER_READ) & 0xFFFF
            return self.tcn2
        if off == 0xE44:
            return self.ievent
        if off == 0xE40:
            return 0                                       # reset completes at once
        return super()._rd(uc, off, size, _)

    def _wr(self, uc, off, size, value, _):
        if off == 0xE44:                                   # IEVENT: write 1 to clear
            self.ievent &= ~value
            return
        super()._wr(uc, off, size, value, _)
        if off == 0xE80:                                   # MII_DATA
            op, phy, reg, data = (value >> 28) & 3, (value >> 23) & 31, (value >> 18) & 31, value & 0xFFFF
            present = phy == PHY_ADDR and self.cable
            if op == 2:                                    # read
                v = self.phy.get(reg, 0) if present else 0xFFFF
                struct.pack_into(">I", self.dpram, 0xE80, (value & 0xFFFF0000) | v)
            elif op == 1 and present:                      # write
                self.phy[reg] = data & ~0x8000 if reg == 0 else data
            self.ievent |= 0x00800000
        elif off == 0xE54:                                 # X_DES_ACTIVE: send ready frames
            base = be32(self.dpram, 0xE14)
            while True:
                bd = base + 8 * self.tx_idx
                st, ln = struct.unpack(">HH", bytes(self.uc.mem_read(bd, 4)))
                if not st & 0x8000:
                    break
                buf = be32(bytes(self.uc.mem_read(bd + 4, 4)), 0)
                self.to_host.append(bytes(self.uc.mem_read(buf, ln)))
                self.uc.mem_write(bd, struct.pack(">H", st & ~0x8000))
                self.tx_idx = 0 if st & 0x2000 else self.tx_idx + 1
            # (no emu_stop here: stopping inside an MMIO write re-runs the store on resume;
            #  the host gets control at the end of each time slice instead)

    def deliver(self):
        """Move queued host frames into empty RX descriptors."""
        base = be32(self.dpram, 0xE10)
        while self.to_dev and base:
            bd = base + 8 * self.rx_idx
            st = struct.unpack(">H", bytes(self.uc.mem_read(bd, 2)))[0]
            if not st & 0x8000:
                return                                     # ring full
            f = self.to_dev.pop(0)
            f = f + b"\0" * max(0, 60 - len(f)) + b"\xde\xad\xbe\xef"   # pad + CRC
            buf = be32(bytes(self.uc.mem_read(bd + 4, 4)), 0)
            self.uc.mem_write(buf, f)
            self.uc.mem_write(bd, struct.pack(">HH", (st & 0x2000) | 0x0800, len(f)))
            self.rx_idx = 0 if st & 0x2000 else self.rx_idx + 1

    def step(self, n=60_000):
        self.deliver()
        pc = self.uc.reg_read(cosim.UC_PPC_REG_PC)
        if self.reached_app:
            return False
        self.uc.emu_start(pc, 0xFFFFFFFF, count=n)
        return not self.reached_app


class Host:
    def __init__(self, board):
        self.b = board
        self.dev_mac = None
        self.seen = []

    def frames(self):
        out, self.b.to_host = self.b.to_host, []
        for f in out:
            self.seen.append(f)
            # answer ARP for our address, like a real host (lwIP re-ARPs after 300 s)
            if f[12:14] == b"\x08\x06" and f[20:22] == b"\0\1" and f[38:42] == HOST_IP:
                self.send(eth(f[6:12], HOST_MAC, 0x0806, struct.pack(
                    ">HHBBH6s4s6s4s", 1, 0x0800, 6, 4, 2, HOST_MAC, HOST_IP, f[22:28], f[28:32])))
            yield f

    def send(self, f):
        self.b.to_dev.append(f)

    def run_until(self, pred, what, limit=3000, retry=None, every=25):
        for n in range(limit):
            if retry and n % every == 0:
                self.send(retry)
            for f in self.frames():
                r = pred(f)
                if r is not None:
                    return r
            if not self.b.step():
                raise SystemExit(f"FAIL: boot continued before {what}")
        raise SystemExit(f"FAIL: timed out waiting for {what}")

    # --- protocol pieces ---
    def udp_payload(self, f, sport=None, dport=None):
        if len(f) < 42 or f[12:14] != b"\x08\x00" or f[23] != 17:
            return None
        ihl = (f[14] & 0xF) * 4
        u = 14 + ihl
        sp, dp, ln = struct.unpack(">HHH", f[u:u + 6])
        if (sport and sp != sport) or (dport and dp != dport):
            return None
        return sp, dp, f[u + 8:u + ln]

    def tftp_get(self, name):
        sport = 40000 + len(self.seen) % 1000
        self.send(eth(self.dev_mac, HOST_MAC, 0x0800,
                      udp(HOST_IP, DEV_IP, sport, 69, b"\0\1" + name.encode() + b"\0octet\0")))
        data, block = b"", 1
        while True:
            def match(f):
                r = self.udp_payload(f, dport=sport)
                if r and r[2][:2] == b"\0\3" and struct.unpack(">H", r[2][2:4])[0] == block:
                    return r
                if r and r[2][:2] == b"\0\5":
                    raise SystemExit(f"FAIL: TFTP error for {name}: {r[2][4:-1]}")
                return None
            sp, _, pl = self.run_until(match, f"TFTP block {block} of {name}")
            data += pl[4:]
            self.send(eth(self.dev_mac, HOST_MAC, 0x0800,
                          udp(HOST_IP, DEV_IP, sport, sp, b"\0\4" + struct.pack(">H", block))))
            if len(pl) - 4 < 512:
                return data
            block += 1

    def tftp_put(self, name, payload):
        sport = 41000
        self.send(eth(self.dev_mac, HOST_MAC, 0x0800,
                      udp(HOST_IP, DEV_IP, sport, 69, b"\0\2" + name.encode() + b"\0octet\0")))
        block = 0
        while True:
            def match(f):
                r = self.udp_payload(f, dport=sport)
                if r and r[2][:2] == b"\0\4" and struct.unpack(">H", r[2][2:4])[0] == block:
                    return r
                if r and r[2][:2] == b"\0\5":
                    raise SystemExit(f"FAIL: TFTP error on put: {r[2][4:-1]}")
                return None
            sp, _, _ = self.run_until(match, f"TFTP ACK {block}")
            chunk = payload[block * 512:(block + 1) * 512]
            if block * 512 > len(payload):
                return
            block += 1
            self.send(eth(self.dev_mac, HOST_MAC, 0x0800,
                          udp(HOST_IP, DEV_IP, sport, sp, b"\0\3" + struct.pack(">H", block) + chunk)))
            if len(chunk) < 512:
                # final ACK, then the bootloader leaves the window and boots
                self.run_until(lambda f: (lambda r: r if r and r[2][:2] == b"\0\4" else None)(
                    self.udp_payload(f, dport=sport)), "final TFTP ACK")
                return


def bootable_entry(dump):
    off = 0
    while True:
        n = be32(dump, off)
        if be32(dump, off + 8) & 1:
            return dump[off:off + n]
        off = (off + n + 3) & ~3


def check_app(b, app):
    ram = bytes(b.uc.mem_read(0, len(app)))
    tp, tl = struct.unpack_from(">II", app, 0x4C)
    return not [i for i in range(0, len(app), 4)
                if not (0x14 <= i < 0x9C or tp <= i < tp + tl) and ram[i:i + 4] != app[i:i + 4]]


def session(title, flash, sc, upload, app, reads=True):
    print(f"\n=== {title} ===")
    b = NetBoard(flash, sc, None)
    h = Host(b)
    ok = True

    def dhcp(f):
        r = h.udp_payload(f, dport=67)
        if r and r[2][0] == 1:
            return f
    f = h.run_until(dhcp, "DHCP discover")
    h.dev_mac = f[6:12]
    print(f"ok   DHCP discover from {h.dev_mac.hex(':')}")

    arp_req = eth(BCAST, HOST_MAC, 0x0806, struct.pack(">HHBBH6s4s6s4s", 1, 0x0800, 6, 4, 1,
                                                       HOST_MAC, HOST_IP, b"\0" * 6, DEV_IP))
    h.run_until(lambda f: f if f[12:14] == b"\x08\x06" and f[20:22] == b"\0\2" and f[28:32] == DEV_IP else None,
                "ARP reply from 169.254.77.1", retry=arp_req)
    print("ok   ARP reply: 169.254.77.1 is at", h.dev_mac.hex(":"), "(static fallback after DHCP)")

    icmp = struct.pack(">BBHHH", 8, 0, 0, 0x1234, 1) + b"marquee-ping"
    icmp = icmp[:2] + struct.pack(">H", csum(icmp)) + icmp[4:]
    h.send(eth(h.dev_mac, HOST_MAC, 0x0800, ipv4(HOST_IP, DEV_IP, 1, icmp)))
    h.run_until(lambda f: f if f[12:14] == b"\x08\x00" and f[23] == 1 and f[34] == 0 else None, "ping reply")
    print("ok   ping reply")

    if reads:
        info = h.tftp_get("info.txt").decode(errors="replace")
        good = "MARQUEE BOOTSTRAP" in info and "169.254.77.1" in info
        ok &= good
        print(("ok  " if good else "FAIL"), "TFTP get info.txt:\n      " + info.strip().replace("\r\n", "\n      "))
        bf = h.tftp_get("bootflash.bin")
        good = bf == flash
        ok &= good
        print(("ok  " if good else "FAIL"), f"TFTP get bootflash.bin: {len(bf)} bytes, identical to the flash: {good}")
        b.dpram[0x956:0x958] = b"\xff\xdf"                  # PA10 pulled to ground: a closed jumper
        t1 = h.tftp_get("inputs.txt").decode()
        b.dpram[0x956:0x958] = b"\xff\xff"                  # jumper removed
        t2 = h.tftp_get("inputs.txt").decode()
        good = "in2 PA10" in t1 and "jumper):  0 (closed" in t1 and "jumper):  1 (open" in t2
        ok &= good
        print(("ok  " if good else "FAIL"), "TFTP get inputs.txt follows a jumper on PA10 live:\n      " +
              t1.strip().replace("\r\n", "\n      "))
        ee = h.tftp_get("eeprom_a0.bin")
        good = ee == cosim.eeprom_cfg(sc["spd"])
        ok &= good
        print(("ok  " if good else "FAIL"), f"TFTP get eeprom_a0.bin: {len(ee)} bytes, equal to the board's EEPROM: {good}")
        nv = h.tftp_get("nvram.bin")
        good = len(nv) == (32 << 10 if not sc["spd"] else 512 << 10)
        ok &= good
        print(("ok  " if good else "FAIL"), f"TFTP get nvram.bin: {len(nv)} bytes")

    name, data = upload
    h.tftp_put(name, data)
    print(f"ok   TFTP put {name}: {len(data)} bytes accepted")
    for _ in range(20000):
        if not b.step(2_000_000):
            break
    good = b.reached_app
    ok &= good
    print(("ok  " if good else "FAIL"), "bootloader jumped to the application at 0")
    if good:
        good = check_app(b, app)
        ok &= good
        print(("ok  " if good else "FAIL"), "app in RAM = ELITE_RGB (booted from the TFTP upload)")
        good = b.dpram[0x972:0x974] == b"\0\0" and be32(b.dpram, 0xE40) == 1
        ok &= good
        print(("ok  " if good else "FAIL"), "Port D and the FEC are back in their reset state before the app runs")
    return ok


def no_cable(title, flash, sc, app):
    print(f"\n=== {title} ===")
    b = NetBoard(flash, sc, None)
    b.cable = False
    steps = 0
    while b.step(2_000_000):
        steps += 1
        if steps > 5000:
            break
    good = b.reached_app and check_app(b, app) and not b.to_host
    print(("ok  " if good else "FAIL"), f"no PHY answers: no frames sent, normal boot from flash "
          f"(app correct: {b.reached_app and check_app(b, app)})")
    return good


def main(net_image, dump_path, app_path):
    sys.path.insert(0, __file__.rsplit("/", 2)[0] + "/tools")
    import mkapp
    flash = open(net_image, "rb").read()
    dump = open(dump_path, "rb").read()
    app = open(app_path, "rb").read()
    t0 = time.time()
    ok = True
    ok &= session("fixed config: compressed ELITE_RGB entry from the dump",
                  flash, cosim.SCENARIOS["fixed config, no upgrade (tries=0xFF)"],
                  ("elite_rgb.img", bootable_entry(dump)), app)
    ok &= session("SPD SDRAM: uncompressed app from tools/mkapp.py",
                  flash, cosim.SCENARIOS["SPD SDRAM, one flash bank + CS7 (tries=0xFF)"],
                  ("app.img", mkapp.entry(app) + b"\0" * 4), app, reads=False)
    ok &= no_cable("fixed config: no PHY / no cable", flash,
                   cosim.SCENARIOS["fixed config, no upgrade (tries=0xFF)"], app)
    print(f"\n{'ALL NETWORK CHECKS PASS' if ok else 'FAILURES'}  ({time.time() - t0:.0f} s wall)")
    return 0 if ok else 1

if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    sys.exit(main(*sys.argv[1:]))
