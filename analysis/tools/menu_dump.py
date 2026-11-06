#!/usr/bin/env python3
"""Rebuild the on-screen menu table (menu_items[192] at 0xDE42C, 0x58 bytes each) from the
constant stores in the named decompile, and print it as a tree.

Usage: menu_dump.py [ghidra_out/elite_rgb_named.c] [--tsv]"""
import re, struct, sys, os

HERE = os.path.dirname(os.path.abspath(__file__))
BASE, SIZE, COUNT = 0xDE42C, 0x58, 192
FIELDS = {0x00: "type", 0x04: "flags", 0x08: "label_short", 0x0C: "label_long", 0x10: "value",
          0x14: "min", 0x18: "max", 0x1C: "action", 0x20: "label_fg", 0x24: "label_bg",
          0x28: "edit_fg", 0x2C: "edit_bg", 0x30: "view_fg", 0x34: "view_bg", 0x38: "options",
          0x3C: "edit_level", 0x40: "view_level", 0x44: "type_arg", 0x48: "hooks", 0x4C: "on_draw",
          0x50: "config_key", 0x54: "key_format"}


def strings(img):
    def s(a):
        return img[a:img.index(b"\0", a)].decode("latin1")
    return s


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    src = open(args[0] if args else os.path.join(HERE, "..", "ghidra_out", "elite_rgb_named.c")).read()
    img = open(os.path.join(HERE, "..", "extracted", "ELITE_RGB"), "rb").read()
    s = strings(img)
    rec = [dict() for _ in range(COUNT)]
    names = {v: k for k, v in FIELDS.items()}
    tnames = {f"menu_types[{i}]": n for i, n in enumerate(
        ["bar", "char", "effect", "readout", "text_b", "choice", "display_config", "number", "number_or_unset", "ranges"])}
    tnames.update({"menu_types_text[0]": "download_line", "menu_types_text[1]": "text"})
    for m in re.finditer(r"menu_items\[(0x[0-9a-f]+|\d+)\]\.(\w+) = ([^;]+);", src):
        i, fld, v = int(m.group(1), 0), m.group(2), m.group(3).strip()
        v = re.sub(r"^\([^)]*\)", "", v).strip()              # drop casts
        v = tnames.get(v, v) if fld == "type" else v
        ms = re.match(r"s_(\w+)_([0-9a-f]{8})$", v)
        if ms:
            v = repr(s(int(ms.group(2), 16)))
        if v == "0x0":
            v = "0"
        rec[i].setdefault(fld, v)
    # submenu descriptors: action of a non-action item -> &DAT_xxx = {items, count, ...}
    syms = {}
    for line in open(os.path.join(HERE, "..", "symbols.tsv")):
        c = line.split("\t")
        if len(c) > 2 and re.match(r"[0-9a-f]{8}$", c[0]):
            syms[c[2]] = int(c[0], 16)

    def sub(v):
        m = re.match(r"&(?:DAT_000(\w+)|(menu_list_\w+))$", v)
        if not m:
            return None
        a = int(m.group(1), 16) if m.group(1) else syms.get(m.group(2), 0)
        if not 0xB0000 <= a < 0xBDDF4:
            return None
        items, n = struct.unpack_from(">II", img, a)
        if BASE <= items < BASE + SIZE * COUNT and (items - BASE) % SIZE == 0 and 0 < n <= COUNT:
            return (items - BASE) // SIZE, n
        return None
    if "--tsv" in sys.argv:
        keys = list(FIELDS.values())
        print("idx\t" + "\t".join(keys))
        for i, r in enumerate(rec):
            print(f"{i}\t" + "\t".join(r.get(k, "") for k in keys))
        return
    bodies = dict((m.group(2), m.group(3)) for m in re.finditer(
        r"/\* === ([0-9a-f]{8})  (\S+) === \*/(.*?)(?=/\* === |\Z)", src, re.S))

    byaddr = {a: n for n, a in syms.items()}

    seen = set()

    def list_addr(v):
        m = re.match(r"&(?:DAT_000(\w+)|(menu_list_\w+))$", v)
        if not m:
            return 0
        a = int(m.group(1), 16) if m.group(1) else syms.get(m.group(2), 0)
        return a if sub(f"&DAT_000{a:05x}") else 0

    def fn_lists(fn):
        """menu_list addresses a function returns or stores (submenu getters, on_open re-pointing)."""
        out = []
        for m in re.finditer(r"&(menu_list_\w+)|\b(0xb5[0-9a-f]{3})\b", bodies.get(fn, "")):
            a = syms.get(m.group(1), 0) if m.group(1) else int(m.group(2), 16)
            if sub(f"&DAT_000{a:05x}") and a not in out:
                out.append(a)
        return out

    def show(i, depth, inherited=()):
        r = rec[i]
        lab = next((r[k] for k in ("label_long", "label_short") if r.get(k, "0") != "0"), "(no label)")
        flags = r.get("flags", "0")
        handler = flags.startswith("0x8")
        act = r.get("action", "")
        extra = [f"{k}={r[k]}" for k in ("type", "config_key", "edit_level", "view_level", "max")
                 if r.get(k) not in (None, "0", "")]
        kids = []
        if not handler and list_addr(act):
            kids = [list_addr(act)]
        elif not handler and act not in ("", "0"):
            kids = fn_lists(act)
            extra.append(f"submenu<-{act}")
        elif handler:
            extra.append(f"handler={act}")
        if not kids and inherited:
            kids = list(inherited)
            extra.append("submenu set by the list's on_open")
        print(f"{'  ' * depth}[{i:3}] {lab:28} {' '.join(extra)}")
        seen.add(i)
        for a in kids:
            if depth >= 10:
                break
            first, n = sub(f"&DAT_000{a:05x}")
            opened = [x for x in fn_lists(byaddr.get(struct.unpack_from(">I", img, a + 12)[0], "")) if x != a]
            for j in range(first, first + n):
                if j not in seen:
                    show(j, depth + 1, opened)
    main = syms.get("menu_list_main")
    if main:
        first, n = struct.unpack_from(">II", img, main)
        first = (first - BASE) // SIZE
        print("menu_list_main (the root menu):")
        for j in range(first, first + n):
            show(j, 1)
        print("not reachable from the root by a static link:")
    for i in range(COUNT):
        if i not in seen:
            show(i, 0)


if __name__ == "__main__":
    main()
