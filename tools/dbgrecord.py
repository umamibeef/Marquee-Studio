"""Decode the debug record our boot loader and firmware keep in NVRAM (bootloader/include/dbg.h).

NVRAM + 0x7C00: a header (magic "MQDB", boot number, options), the last 8 boots (reset cause, last step reached),
and the last crash (who caught it, vector, SRR0/SRR1/DAR/DSISR/LR/CR, r0..r31, the step it happened in).
"""
import struct

OFFSET = 0x7C00
SIZE = 0x100 + 0x30 + 32 * 4
MAGIC, CRASH_MAGIC = 0x4D514442, 0x4D514352
OPT_SLOT_DIRECT = 0x01

STEPS = {
    0x01: "boot loader started", 0x02: "SDRAM up (exceptions caught)", 0x03: "serial window closed",
    0x04: "stage 1 running (from the main flash)", 0x05: "stage 1 skipped (1 recovery, 2 bad image, 3 tries)",
    0x10: "update slot found", 0x11: "trying the slot", 0x12: "slot copied to the RAM stage",
    0x13: "booting the slot in place (direct mode)", 0x18: "booting the boot flash's image",
    0x19: "booting the staged image (RUN)", 0x20: "loading the app", 0x21: "app loaded", 0x22: "memory map set",
    0x23: "handed off to the app", 0x40: "firmware started", 0x41: "firmware exception table (crash reports on)",
    0x42: "firmware main task started", 0x43: "firmware memory layout (main flash mapped)",
    0x4F: "firmware healthy (1200 frames)",
}
SOURCES = {1: "boot loader exception", 2: "firmware DTLB miss with no mapping (would hang)", 3: "firmware crash"}
VECTORS = {0x100: "system reset", 0x200: "machine check", 0x300: "DSI (data access)", 0x400: "ISI (instruction fetch)",
           0x500: "external interrupt", 0x600: "alignment", 0x700: "program (trap/illegal)", 0x800: "FP unavailable",
           0x900: "decrementer", 0xC00: "system call", 0xD00: "trace", 0x1000: "software emulation",
           0x1100: "ITLB miss", 0x1200: "DTLB miss", 0x1300: "ITLB error", 0x1400: "DTLB error"}
RSR_BITS = [(0x80000000, "ext-hard"), (0x40000000, "ext-soft"), (0x20000000, "loss-of-lock"), (0x10000000, "sw-wdog"),
            (0x08000000, "checkstop"), (0x04000000, "debug-hard"), (0x02000000, "debug-soft"), (0x01000000, "jtag")]


def rsr_text(v):
    names = [n for bit, n in RSR_BITS if v & bit]
    return " ".join(names) if names else "-"


def step_text(s):
    return STEPS.get(s, f"step {s:#x}")


def decode(blob):
    """The record as a dict, or None if it was never started."""
    magic, seq, options, s1_tries = struct.unpack_from(">IIBB", blob, 0)
    if magic != MAGIC:
        return None
    boots = []
    for i in range(8):
        bseq, rsr, step, _, value = struct.unpack_from(">IIHHI", blob, 0x10 + 16 * i)
        if bseq:
            boots.append({"seq": bseq, "rsr": rsr, "step": step, "value": value})
    boots.sort(key=lambda b: b["seq"])
    c = struct.unpack_from(">12I", blob, 0x100)
    crash = None
    if c[0] == CRASH_MAGIC:
        names = ["magic", "seq", "source", "vector", "srr0", "srr1", "dar", "dsisr", "lr", "cr", "step", "value"]
        crash = dict(zip(names, c))
        crash["gpr"] = list(struct.unpack_from(">32I", blob, 0x130))
    return {"seq": seq, "options": options, "s1_tries": s1_tries, "boots": boots, "crash": crash}


def report(rec):
    if rec is None:
        return "no debug record yet (our boot loader starts it on its first boot)"
    out = [f"boot #{rec['seq']}, options {rec['options']:#04x}, stage-1 tries {rec['s1_tries']}"
           f" (slot boot: {'direct, in place' if rec['options'] & OPT_SLOT_DIRECT else 'through the RAM stage'})",
           "last boots (oldest first): reset cause -> last step reached"]
    for b in rec["boots"]:
        cur = "  <- this boot" if b["seq"] == rec["seq"] else ""
        out.append(f"  #{b['seq']:<5} {rsr_text(b['rsr']):<28} -> {step_text(b['step'])}"
                   f" ({b['value']:#010x}){cur}")
    c = rec["crash"]
    if c is None:
        out.append("no crash recorded")
    else:
        vec = VECTORS.get(c["vector"], "")
        out.append(f"last crash: boot #{c['seq']}, {SOURCES.get(c['source'], c['source'])},"
                   f" vector {c['vector']:#x} {vec}")
        out.append(f"  during: {step_text(c['step'])} ({c['value']:#010x})")
        out.append(f"  srr0 {c['srr0']:08x}  srr1 {c['srr1']:08x}  dar {c['dar']:08x}  dsisr {c['dsisr']:08x}"
                   f"  lr {c['lr']:08x}  cr {c['cr']:08x}")
        g = c["gpr"]
        for r in range(0, 32, 8):
            out.append("  " + " ".join(f"r{r + i:<2} {g[r + i]:08x}" for i in range(8)))
    return "\n".join(out)
