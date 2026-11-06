#!/usr/bin/env python3
"""Build an app bundle (docs/APPS.md) from an app's C source: clang -> eBPF ELF object -> MQAP bundle.

    mkapp.py apps/clock OUT.mqap [key=value ...]     the app with its default settings (app.json), overridden by any
                                                      key=value given (the Studio passes an instance's settings)

MQAP1, big-endian header, little-endian eBPF code:
    header    40 bytes: "MQAP", abi u16 (1), flags u16 (0), code bytes u32, data bytes u32, bss bytes u32,
              init instruction u32, frame instruction u32, settings bytes u32, assets bytes u32, manifest bytes u32
    code      the instructions; every 64-bit load of an address already points into the data segment
    data      the initialised data (.rodata, .data); the bss follows it in the app's memory, zeroed
    settings  key '=' value NUL, ...: the instance's settings (Latin-1 text, native emoji as UTF-8)
    assets    count u16, then each: name length u8, name, width u16, height u16, frames u16, ms per frame u16,
              compressed bytes u32, zlib(pixels: RGB555 u16 per pixel, bit 15 set = transparent, frame after frame)
    manifest  app.json (the Studio's description of the app: title, settings, feed)
The data segment is at DATA_VA in the app's address space, so every relocation is resolved here.
"""
import json, os, shutil, struct, subprocess, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def _find_clang():
    """BPF_CLANG, else the Mac's Homebrew LLVM (has the BPF backend + mcpu=v4), else clang-19/18 on PATH (the
    container). Apple's /usr/bin/clang lacks the BPF backend, so PATH 'clang' is a last resort only."""
    if os.environ.get("BPF_CLANG"):
        return os.environ["BPF_CLANG"]
    hb = "/opt/homebrew/opt/llvm/bin/clang"
    if os.path.exists(hb):
        return hb
    for c in ("clang-19", "clang-18", "clang"):
        if shutil.which(c):
            return shutil.which(c)
    return hb


CLANG = _find_clang()
DATA_VA = 0x20000000
ABI = 1


# ---- a small ELF64 (little-endian) reader: sections, symbols, REL relocations ----
class Elf:
    def __init__(self, data):
        if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
            raise ValueError("not a little-endian ELF64 object")
        self.d = data
        shoff, = struct.unpack_from("<Q", data, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
        self.sec = []
        for i in range(shnum):
            name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from(
                "<IIQQQQIIQQ", data, shoff + i * shentsize)
            self.sec.append(dict(name=name, type=typ, flags=flags, off=off, size=size, link=link, info=info,
                                 align=max(1, align), entsize=entsize))
        names = self.sec[shstrndx]
        for s in self.sec:
            s["name"] = self.cstr(names["off"] + s["name"])
        self.syms = []
        for s in self.sec:
            if s["type"] == 2:                                   # SHT_SYMTAB
                strtab = self.sec[s["link"]]
                for k in range(s["size"] // 24):
                    nm, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", data, s["off"] + 24 * k)
                    self.syms.append(dict(name=self.cstr(strtab["off"] + nm), info=info, shndx=shndx, value=value))

    def cstr(self, off):
        return self.d[off:self.d.index(b"\0", off)].decode()

    def body(self, s):
        return self.d[s["off"]:s["off"] + s["size"]]


def compile_app(src, obj):
    subprocess.run([CLANG, "-target", "bpfel", "-mcpu=v4", "-O2", "-Wall", "-Werror", "-fno-builtin",
                    "-I", os.path.join(REPO, "apps", "include"), "-c", src, "-o", obj], check=True)


def link(elf):
    """(code bytes, data bytes, bss size, {symbol: instruction}) with every relocation resolved"""
    text = next((i for i, s in enumerate(elf.sec) if s["name"] == ".text"), None)
    if text is None:
        raise ValueError("no .text")
    for i, s in enumerate(elf.sec):                              # code only in .text (no -ffunction-sections)
        if s["flags"] & 0x4 and i != text and s["size"]:
            raise ValueError(f"code in {s['name']}: keep every function in .text")
    code = bytearray(elf.body(elf.sec[text]))
    # the data segment: progbits first, then nobits, each at its alignment
    base, data, bss_end = {}, bytearray(), 0
    alloc = [(i, s) for i, s in enumerate(elf.sec) if s["flags"] & 0x2 and not s["flags"] & 0x4 and s["size"]]
    for i, s in alloc:
        if s["type"] != 8:                                       # PROGBITS
            data += b"\0" * (-len(data) % s["align"])
            base[i] = len(data)
            data += elf.body(s)
    end = len(data)
    for i, s in alloc:
        if s["type"] == 8:                                       # NOBITS (.bss)
            end += -end % s["align"]
            base[i] = end
            end += s["size"]
    bss = end - len(data)

    def target(sym, addend):
        if sym["shndx"] not in base:
            raise ValueError(f"relocation against {sym['name'] or elf.sec[sym['shndx']]['name']}: not data")
        return DATA_VA + base[sym["shndx"]] + sym["value"] + addend

    for rs in elf.sec:
        if rs["type"] != 9:                                      # SHT_REL
            continue
        tgt = rs["info"]
        symtab = rs["link"]
        for k in range(rs["size"] // 16):
            off, info = struct.unpack_from("<QQ", elf.d, rs["off"] + 16 * k)
            sym, typ = elf.syms[info >> 32], info & 0xFFFFFFFF
            if tgt == text and typ == 1:                         # R_BPF_64_64: lddw of an address
                lo, = struct.unpack_from("<I", code, off + 4)
                hi, = struct.unpack_from("<I", code, off + 12)
                v = target(sym, lo | hi << 32)
                struct.pack_into("<I", code, off + 4, v & 0xFFFFFFFF)
                struct.pack_into("<I", code, off + 12, v >> 32)
            elif tgt == text and typ == 10:                      # R_BPF_64_32: a call to a function in .text
                if sym["shndx"] != text:
                    raise ValueError(f"call to {sym['name']}: not in .text")
                imm, = struct.unpack_from("<i", code, off + 4)
                dest = sym["value"] // 8 + (imm + 1 if (sym["info"] & 0xF) == 3 else 0)   # section symbol: imm addend
                struct.pack_into("<i", code, off + 4, dest - off // 8 - 1)
            elif tgt in base and typ in (2, 3):                  # R_BPF_64_ABS64 / ABS32 in data: a pointer
                at = base[tgt] + off
                n = 8 if typ == 2 else 4
                add = int.from_bytes(data[at:at + n], "little")
                data[at:at + n] = target(sym, add).to_bytes(n, "little")
            elif tgt in base and typ == 10:
                raise ValueError("function pointers in data are not supported")
            else:
                raise ValueError(f"relocation type {typ} in {elf.sec[tgt]['name']}")
    entry = {}
    for s in elf.syms:
        if s["shndx"] == text and (s["info"] & 0xF) == 2 and s["info"] >> 4 == 1:   # global functions
            entry[s["name"]] = s["value"] // 8
    return bytes(code), bytes(data), bss, entry


def encode_text(s):
    """settings text as the sign draws it: Latin-1, native emoji as UTF-8 (studio native_emoji.encode)"""
    sys.path.insert(0, REPO)
    from marquee_pack import native_emoji
    return native_emoji.encode(s)


def settings_bytes(defaults, overrides):
    vals = dict(defaults)
    vals.update(overrides)
    out = b""
    for k, v in vals.items():
        if isinstance(v, bool):
            v = int(v)
        out += k.encode("ascii") + b"=" + (str(v).encode() if isinstance(v, int) else encode_text(str(v))) + b"\0"
    return out


def _frames(path):
    """[(RGBA frames)], ms per frame of a picture or animation: PNG / GIF (Pillow) or FLC (the sign's own format,
    decoded by the studio's FLIC)"""
    from PIL import Image, ImageSequence
    if path.lower().endswith(".flc"):
        sys.path.insert(0, REPO)
        from marquee_pack.sign_emu import FLIC
        from marquee_pack.sign_emu import sign_frame_ms
        fl = FLIC(open(path, "rb").read())
        out = []
        for rows in fl.frames_rgb()[:fl.frames]:
            im = Image.new("RGBA", (fl.w, fl.h))
            im.putdata([(r, g, b, 255) for row in rows for (r, g, b) in row])
            out.append(im)
        return out, sign_frame_ms(fl.speed_ms)
    im = Image.open(path)
    return [f.convert("RGBA") for f in ImageSequence.Iterator(im)], int(im.info.get("duration", 100) or 100)


def assets_bytes(app_dir, extra=None):
    """the app's assets/ folder (PNG / GIF / FLC) and any extra assets ({name: path}), as RGB555 frames (bit 15 set
    = transparent), each zlib-compressed:
        count u16, then each: name length u8, name, width u16, height u16, frames u16, ms per frame u16,
        compressed bytes u32, zlib(pixels, frame after frame, big-endian u16)"""
    import zlib
    d = os.path.join(app_dir, "assets")
    items = {n.rsplit(".", 1)[0]: os.path.join(d, n) for n in sorted(os.listdir(d))
             if n.lower().endswith((".png", ".gif", ".flc"))} if os.path.isdir(d) else {}
    items.update(extra or {})
    out = struct.pack(">H", len(items))
    for name, path in items.items():
        frames, ms = _frames(path)
        w, h = frames[0].size
        px = bytearray()
        for f in frames:
            for r, g, b, a in f.getdata():
                px += struct.pack(">H", 0x8000 if a < 128 else (r >> 3) << 10 | (g >> 3) << 5 | b >> 3)
        z = zlib.compress(bytes(px), 9)
        nm = name.encode()
        out += struct.pack(">B", len(nm)) + nm + struct.pack(">HHHHI", w, h, len(frames), ms, len(z)) + z
    return out


def build(app_dir, overrides=None, out=None, extra_assets=None):
    app_dir = os.path.abspath(app_dir)
    manifest = json.load(open(os.path.join(app_dir, "app.json"), encoding="utf-8"))
    obj = (out or os.path.join(app_dir, "app")) + ".o"
    compile_app(os.path.join(app_dir, "app.c"), obj)
    code, data, bss, entry = link(Elf(open(obj, "rb").read()))
    os.remove(obj)
    for need in ("init", "frame"):
        if need not in entry:
            raise ValueError(f"{app_dir}: no {need}() function")
    manifest["name"] = os.path.basename(app_dir)
    defaults = {s["key"]: s.get("default", "") for s in manifest.get("settings", [])}
    st = settings_bytes(defaults, overrides or {})
    assets = assets_bytes(app_dir, extra_assets)
    man = json.dumps(manifest, ensure_ascii=False).encode()
    head = b"MQAP" + struct.pack(">HHIIIIIIII", ABI, 0, len(code), len(data), bss, entry["init"], entry["frame"],
                                 len(st), len(assets), len(man))
    bundle = head + code + data + st + assets + man
    if out:
        with open(out, "wb") as f:
            f.write(bundle)
    return bundle


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    ov = dict(a.split("=", 1) for a in sys.argv[3:])
    b = build(sys.argv[1], ov, sys.argv[2])
    print(f"{sys.argv[2]}: {len(b)} bytes")
