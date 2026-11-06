#!/usr/bin/env python3
"""An eBPF assembler for the syntax of the bpf-conformance test programs (third_party/bpf-conformance/tests), with the
encodings of RFC 9669 (the same ones as bpf-conformance's lib/src/bpf_assembler.cc). Little-endian instructions:
opcode, dst | src << 4, offset (s16), imm (s32).

    ebpf_asm.py --fixtures OUT.tsv      assemble every conformance test into the interpreters' fixture file
"""
import os, re, struct, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TESTS = os.path.join(REPO, "third_party", "bpf-conformance", "tests")

ALU = {"add": 0, "sub": 1, "mul": 2, "div": 3, "or": 4, "and": 5, "lsh": 6, "rsh": 7, "neg": 8, "mod": 9, "xor": 10,
       "mov": 11, "arsh": 12, "sdiv": 3, "smod": 9}
JMP = {"jeq": 1, "jgt": 2, "jge": 3, "jset": 4, "jne": 5, "jsgt": 6, "jsge": 7, "jlt": 10, "jle": 11, "jslt": 12,
       "jsle": 13}
SIZE = {"w": 0x00, "h": 0x08, "b": 0x10, "dw": 0x18}
ATOMIC = {"add": 0x00, "or": 0x40, "and": 0x50, "xor": 0xa0}


def ins(op, dst=0, src=0, off=0, imm=0):
    return struct.pack("<BBhi", op, (dst & 15) | (src & 15) << 4, off, imm if imm < 0x80000000 else imm - (1 << 32))


def reg(s):
    m = re.fullmatch(r"%r(\d+)", s.strip())
    if not m:
        raise ValueError(f"not a register: {s!r}")
    return int(m.group(1))


def num(s):
    s = s.strip()
    v = int(s, 0)
    return v


def imm32(s):
    v = num(s)
    if not -(1 << 31) <= v < (1 << 32):
        raise ValueError(f"immediate out of range: {s}")
    return v & 0xFFFFFFFF


def memref(s):
    m = re.fullmatch(r"\[\s*%r(\d+)\s*(?:([+-])\s*(\w+))?\s*\]", s.strip())
    if not m:
        raise ValueError(f"not a memory operand: {s!r}")
    off = int(m.group(3), 0) if m.group(3) else 0
    return int(m.group(1)), -off if m.group(2) == "-" else off


def split_ops(rest):
    out, depth, cur = [], 0, ""
    for ch in rest:
        if ch == "[":
            depth += 1
        elif ch == "]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def assemble(text):
    """eBPF bytecode of a program in the conformance syntax (labels, '+N' jump offsets, comments with '#')."""
    lines = []
    for ln in text.split("\n"):
        ln = ln.split("#", 1)[0].strip()
        if ln:
            lines.append(ln)
    labels, items, pc, exits = {}, [], 0, {}
    for ln in lines:                                   # first pass: labels and instruction slots
        while True:
            m = re.match(r"^([A-Za-z_]\w*):\s*(.*)$", ln)
            if not m:
                break
            labels[m.group(1)] = pc
            ln = m.group(2)
        if not ln:
            continue
        mn = ln.split(None, 1)[0]
        if mn == "exit" and "exit" not in exits:          # the first exit is also the label "exit" (as the suite's own
            exits["exit"] = pc                            # assembler does)
        items.append((pc, ln))
        pc += 2 if mn == "lddw" else 1

    for k, v in exits.items():
        labels.setdefault(k, v)

    def target(s, here):
        s = s.strip()
        if s in labels:
            return labels[s] - here - 1
        return num(s)

    out = b""
    for here, ln in items:
        parts = ln.split(None, 1)
        mn, ops = parts[0], split_ops(parts[1]) if len(parts) > 1 else []
        if mn == "exit":
            out += ins(0x95)
        elif mn == "lddw":
            v = num(ops[1]) & 0xFFFFFFFFFFFFFFFF
            out += ins(0x18, reg(ops[0]), 0, 0, v & 0xFFFFFFFF) + ins(0, 0, 0, 0, v >> 32)
        elif mn == "ja":
            out += ins(0x05, off=target(ops[0], here))
        elif mn == "ja32":
            out += ins(0x06, imm=target(ops[0], here) & 0xFFFFFFFF)
        elif mn == "call":
            o = parts[1].split()
            if o[0] == "local":
                out += ins(0x85, 0, 1, 0, target(o[1], here) & 0xFFFFFFFF)
            elif o[0].startswith("%"):
                out += ins(0x8d, reg(o[0]))
            else:
                out += ins(0x85, 0, 0, 0, imm32(o[0]))
        elif mn == "lock":
            o = parts[1].split(None, 1)
            fetch = o[0] == "fetch"
            if fetch:
                o = o[1].split(None, 1)
            op, rest = o[0], split_ops(o[1])
            w = op.endswith("32")
            base = op[:-2] if w else op
            d, off = memref(rest[0])
            if base == "xchg":
                code = 0xe1
            elif base == "cmpxchg":
                code = 0xf1
            else:
                code = ATOMIC[base] | (1 if fetch else 0)
            out += ins(0xc3 | (0x00 if w else 0x18), d, reg(rest[1]), off, code)
        elif re.fullmatch(r"(ldx|ldxs)(w|h|b|dw)", mn):
            m = re.fullmatch(r"(ldxs?)(w|h|b|dw)", mn)
            mode = 0x80 if m.group(1) == "ldxs" else 0x60
            s, off = memref(ops[1])
            out += ins(0x01 | mode | SIZE[m.group(2)], reg(ops[0]), s, off)
        elif re.fullmatch(r"st(w|h|b|dw)", mn):
            d, off = memref(ops[0])
            out += ins(0x62 | SIZE[mn[2:]], d, 0, off, imm32(ops[1]))
        elif re.fullmatch(r"stx(w|h|b|dw)", mn):
            d, off = memref(ops[0])
            out += ins(0x63 | SIZE[mn[3:]], d, reg(ops[1]), off)
        elif re.fullmatch(r"(le|be)(16|32|64)", mn):
            out += ins(0xd4 if mn.startswith("le") else 0xdc, reg(ops[0]), imm=int(mn[2:]))
        elif re.fullmatch(r"b?swap(16|32|64)", mn):
            out += ins(0xd7, reg(ops[0]), imm=int(re.sub(r"\D", "", mn)))
        elif re.fullmatch(r"movsx(8|16|32)(32|64)", mn):
            m = re.fullmatch(r"movsx(8|16|32)(32|64)", mn)
            cls = 0x04 if m.group(2) == "32" else 0x07
            out += ins(cls | 0x08 | 0xb0, reg(ops[0]), reg(ops[1]), int(m.group(1)))
        elif (mn[:-2] if mn.endswith("32") else mn) in JMP:
            w = mn.endswith("32")
            base = mn[:-2] if w else mn
            cls = 0x06 if w else 0x05
            off = target(ops[2], here)
            if ops[1].startswith("%"):
                out += ins(cls | 0x08 | JMP[base] << 4, reg(ops[0]), reg(ops[1]), off)
            else:
                out += ins(cls | JMP[base] << 4, reg(ops[0]), 0, off, imm32(ops[1]))
        else:
            w = mn.endswith("32")
            base = mn[:-2] if w else mn
            if base not in ALU:
                raise ValueError(f"unknown instruction {mn!r}")
            cls = 0x04 if w else 0x07
            off = 1 if base in ("sdiv", "smod") else 0
            if base == "neg":
                out += ins(cls | 0x80, reg(ops[0]))
            elif ops[1].startswith("%"):
                out += ins(cls | 0x08 | ALU[base] << 4, reg(ops[0]), reg(ops[1]), off)
            else:
                out += ins(cls | ALU[base] << 4, reg(ops[0]), 0, off, imm32(ops[1]))
    return out


def parse_test(path):
    """{section: text} of a .data test file"""
    sec, cur = {}, None
    for ln in open(path, encoding="utf-8"):
        if ln.startswith("-- "):
            cur = ln[3:].strip()
            sec[cur] = ""
        elif cur:
            sec[cur] += ln
    return sec


def test_case(path):
    """(code bytes, mem bytes, expected result) of one conformance test"""
    s = parse_test(path)
    if "raw" in s:
        code = b"".join(struct.pack("<Q", int(w, 16)) for w in s["raw"].split())
    else:
        code = assemble(s["asm"])
    mem = bytes.fromhex("".join(s.get("mem", "").split()))
    result = int(s["result"].split()[0], 16) & 0xFFFFFFFFFFFFFFFF
    return code, mem, result


def fixtures(out):
    rows = []
    for f in sorted(os.listdir(TESTS)):
        if not f.endswith(".data"):
            continue
        code, mem, result = test_case(os.path.join(TESTS, f))
        rows.append(f"{f[:-5]}\t{code.hex()}\t{mem.hex() or '-'}\t{result:016x}")
    with open(out, "w") as fh:
        fh.write("# name\tcode (little-endian eBPF)\tmem (r1 points at it, r2 = its length)\texpected r0\n")
        fh.write("\n".join(rows) + "\n")
    return len(rows)


if __name__ == "__main__":
    if sys.argv[1:2] == ["--fixtures"]:
        print(fixtures(sys.argv[2]), "tests")
    else:
        sys.stdout.write(assemble(sys.stdin.read()).hex() + "\n")
