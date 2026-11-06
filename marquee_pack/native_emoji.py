"""Native emoji: the content file for firmware with native emoji support (firmware/ext/native_emoji.c).

The firmware draws an emoji wherever a text contains its UTF-8 bytes, from one shared set, whatever the font. The
set travels as one file, emoji.bin, packed small:
  header   ">IHHHHII": magic "EMJ2", cell size, count, colours, frames, map length, unpacked length (20 bytes)
  zlib     the palette (colours x RGB555, entry 0 transparent), the map (one emoji per line, UTF-8, without
           U+FE0F, in cell order), the cell table (count x ">HHH": first frame, frames, ms per frame) and the
           pixels (frames pictures of size x size palette indices; a cell's frames are consecutive)
An "EMJ1" file (no cell table, one frame per cell, the 5th header word 0) still loads.
The cells are as tall as the panel (SIZE by default) and the firmware draws them 1:1, centred on the text line. An
animated cell shows frame (t * 25 // ms) % frames, t being the line's (or the app's) step count, 25 ms each.

The pictures are pixel art drawn at the cell size, so they show exactly as drawn:
  - the emoji pack, when it is on this machine (paths.EMOJI_PACK: JOI3's Flat Pixel Art Emojis, bought;
    Unicode_Map.json and Emojis_Flat_<n>x<n>/ folders). The pack's files are not committed;
  - otherwise the committed build (firmware/emoji/<size>/emoji.bin), which `update` makes from the pack.
The set is firmware/emoji/set.txt, then the user's own emoji (custom(): GIFs imported into the studio's content
folder, CUSTOM_DIR, each with a code point of Unicode's private use area, U+E000 up). An emoji is one proportional glyph as wide as a cell; plain text keeps its
one-byte-per-character Latin-1 encoding. encode() makes the bytes to send.
"""
import glob
import os
import re
import struct
import sys
import zlib
from collections import Counter

from PIL import Image

from . import paths

SIZE = 16           # the panel height when none is given (the studio's default panel; the real sign is unmeasured)
MAGIC = 0x454D4A32  # "EMJ2"
MAGIC1 = 0x454D4A31  # "EMJ1": no cell table
HEADER = ">IHHHHII"
STEP_MS = 25        # one step of a text line (or an app), the clock of animated emoji
MAX_FRAMES = 64     # frames per animated emoji
FIRST_CUSTOM = 0xE000
CUSTOM_DIR = os.path.join(paths.CONTENT, "emoji")
EMOJI_DIR = os.path.join(paths.REPO, "firmware", "emoji")
CREDIT = "Emoji graphics from JOI3's Flat Pixel Art Emojis (https://joi3.itch.io/flat-pixel-art-emojis)."


def key(e):
    """The map form of an emoji: without U+FE0F."""
    return "".join(c for c in e if c != "\uFE0F")


def _load_pack():
    """{emoji: picture id} from the emoji pack's Unicode_Map.json, or {} when the pack is not here. Read leniently
    (the file as shipped misses a comma); a picture shared by many codes (the pack's generic flag), and ids with no
    picture, are left out."""
    p = os.path.join(paths.EMOJI_PACK, "Unicode_Map.json")
    if not os.path.exists(p):
        return {}
    with open(p, encoding="utf-8") as f:
        pairs = re.findall(r'"unicode":\s*"([^"]+)",\s*"pixel_emoji_id":\s*"(\d+)"', f.read())
    shared = Counter(i for _, i in pairs)
    drawn = os.path.join(paths.EMOJI_PACK, "Emojis_Flat_16x16", "Emojis_Flat_16x16_{}.png")
    out = {}
    for u, i in pairs:
        e = key("".join(chr(int(c[2:], 16)) for c in u.split()))
        if shared[i] <= 3 and e not in out and os.path.exists(drawn.format(i)):   # the map names a few missing ids
            out[e] = int(i)
    return out


def _read_set():
    """The emoji of firmware/emoji/set.txt, in order."""
    with open(os.path.join(EMOJI_DIR, "set.txt"), encoding="utf-8") as f:
        return tuple(e for line in f if not line.startswith("#") for e in line.split())


PACK = _load_pack()
DEFAULT = _read_set()


# ---- the user's own emoji: imported GIFs (or PNG, APNG, WebP), each a private-use code point ----
def _custom_file():
    return os.path.join(CUSTOM_DIR, "custom.json")


def custom():
    """[{"e": the character, "cp": "E000", "name", "file"}] of the imported emoji, in code point order."""
    import json
    try:
        with open(_custom_file(), encoding="utf-8") as f:
            items = json.load(f)
    except (OSError, ValueError):
        return []
    out = [{**c, "e": chr(int(c["cp"], 16))} for c in items
           if os.path.exists(os.path.join(CUSTOM_DIR, c.get("file", "")))]
    return sorted(out, key=lambda c: int(c["cp"], 16))


def _custom_stamp():
    try:
        return os.stat(_custom_file()).st_mtime_ns
    except OSError:
        return 0


def _save_custom(items):
    import json
    os.makedirs(CUSTOM_DIR, exist_ok=True)
    with open(_custom_file(), "w", encoding="utf-8") as f:
        json.dump([{k: c[k] for k in ("cp", "name", "file")} for c in items], f, indent=1)


def add_custom(data, name):
    """Imports a picture (bytes of a GIF, PNG, APNG or WebP) as a new emoji; returns its entry. Raises ValueError
    when it is not a picture."""
    import io
    try:
        im = Image.open(io.BytesIO(data))
        im.load()
    except Exception as e:
        raise ValueError(f"not a picture: {e}") from None
    items = custom()
    used = {int(c["cp"], 16) for c in items}
    cp = next(c for c in range(FIRST_CUSTOM, 0xF900) if c not in used)
    ext = {"GIF": "gif", "PNG": "png", "WEBP": "webp"}.get(im.format, "png")
    fname = f"{cp:04X}.{ext}"
    os.makedirs(CUSTOM_DIR, exist_ok=True)
    with open(os.path.join(CUSTOM_DIR, fname), "wb") as f:
        f.write(data)
    entry = {"cp": f"{cp:04X}", "name": (name or "").strip() or f"custom {cp - FIRST_CUSTOM + 1}", "file": fname}
    _save_custom(items + [entry])
    return {**entry, "e": chr(cp)}


def remove_custom(cp):
    """Removes an imported emoji (cp: "E000"); True if it was there."""
    items = custom()
    keep = [c for c in items if c["cp"].upper() != cp.upper()]
    for c in items:
        if c not in keep:
            try:
                os.remove(os.path.join(CUSTOM_DIR, c["file"]))
            except OSError:
                pass
    _save_custom(keep)
    return len(keep) != len(items)


def _custom_by_key():
    return {c["e"]: c for c in custom()}


def full_set():
    """The sign's set: firmware/emoji/set.txt, then the imported emoji."""
    return DEFAULT + tuple(c["e"] for c in custom())


def _fit(frame, size):
    """An RGBA picture in a size x size cell: pixel art no bigger than the cell at the largest whole scale, a bigger
    picture shrunk to fit (aspect kept); centred."""
    w, h = frame.size
    if w <= size and h <= size:
        k = max(1, size // max(w, h))
        art = frame.resize((w * k, h * k), Image.NEAREST)
    else:
        k = size / max(w, h)
        art = frame.resize((max(1, round(w * k)), max(1, round(h * k))), Image.LANCZOS)
    cell = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    cell.alpha_composite(art, ((size - art.width) // 2, (size - art.height) // 2))
    return cell


def custom_frames(path, size):
    """([RGBA cell], ms per frame) of an imported picture. A frame's own duration becomes a whole number of
    frames of the shortest one (at least two steps, 50 ms); at most MAX_FRAMES frames, slowed down to fit."""
    from PIL import ImageSequence
    im = Image.open(path)
    frames, durations = [], []
    for fr in ImageSequence.Iterator(im):
        frames.append(_fit(fr.convert("RGBA"), size))
        durations.append(int(fr.info.get("duration") or 100))
    if len(frames) == 1:
        return frames, 0
    base = max(2 * STEP_MS, min(durations))
    while True:
        reps = [max(1, round(d / base)) for d in durations]
        if sum(reps) <= MAX_FRAMES:
            break
        base = base * sum(reps) // MAX_FRAMES + 1
    return [f for f, r in zip(frames, reps) for _ in range(r)], base


def unpack_frames(data):
    """(size, palette, [emoji], [(ms, [frame pixel bytes])]) of an emoji.bin (EMJ2, or EMJ1: one frame each)."""
    magic, size, count, colours, frames, map_len, raw_len = struct.unpack_from(HEADER, data)
    if magic not in (MAGIC, MAGIC1):
        raise ValueError("not an emoji.bin")
    raw = zlib.decompress(data[struct.calcsize(HEADER):])
    palette = struct.unpack_from(f">{colours}H", raw)
    emojis = raw[colours * 2:colours * 2 + map_len].decode("utf-8").split("\n")[:count]
    at, n2 = colours * 2 + map_len, size * size
    if magic == MAGIC1:
        table = [(n, 1, 0) for n in range(count)]
    else:
        table = [struct.unpack_from(">HHH", raw, at + 6 * n) for n in range(count)]
        at += 6 * count
    px = raw[at:]
    cells = [(ms, [px[(f0 + k) * n2:(f0 + k + 1) * n2] for k in range(nf)]) for f0, nf, ms in table]
    return size, palette, emojis, cells


def unpack(data):
    """(size, palette, [emoji], [cell pixel bytes, frame 0]) of an emoji.bin."""
    size, palette, emojis, cells = unpack_frames(data)
    return size, palette, emojis, [fr[0] for _, fr in cells]


_built = {}


def _committed(size):
    """{emoji: RGBA cell} from the committed firmware/emoji/<size>/emoji.bin, or {}."""
    if size not in _built:
        p = os.path.join(EMOJI_DIR, str(size), "emoji.bin")
        cells = {}
        if os.path.exists(p):
            with open(p, "rb") as f:
                s, palette, emojis, px = unpack(f.read())
            for e, c in zip(emojis, px):          # frame 0
                img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
                img.putdata([(0, 0, 0, 0) if not v else ((palette[v] >> 10 & 31) << 3, (palette[v] >> 5 & 31) << 3,
                                                         (palette[v] & 31) << 3, 255) for v in c])
                cells[e] = img
        _built[size] = cells
    return _built[size]


def has_picture(e, size=SIZE):
    k = key(e)
    if k in _custom_by_key():
        return True
    return k in PACK if PACK else k in _committed(size)


def available(emojis=None, size=SIZE):
    """The emoji of the list (the sign's set, full_set(), by default) that have a picture, in order, without
    duplicates."""
    emojis = full_set() if emojis is None else emojis
    seen, out = set(), []
    for e in emojis:
        k = key(e)
        if k not in seen and has_picture(e, size):
            seen.add(k)
            out.append(k)
    return out


def made_for(e, size):
    """An emoji for a size x size cell: pixel art drawn at (or below) that size, as drawn, at the largest whole
    scale that fits, centred; a cell smaller than any drawing gets the middle of the picture."""
    c = _custom_by_key().get(key(e))
    if c:
        return custom_frames(os.path.join(CUSTOM_DIR, c["file"]), size)[0][0]
    if not PACK:
        return _committed(size)[key(e)]
    drawn = sorted(int(re.search(r"(\d+)x\1$", d).group(1))
                   for d in glob.glob(os.path.join(paths.EMOJI_PACK, "Emojis_Flat_*x*")))
    n = max([d for d in drawn if d <= size] or drawn[:1])
    art = Image.open(os.path.join(paths.EMOJI_PACK, f"Emojis_Flat_{n}x{n}",
                                  f"Emojis_Flat_{n}x{n}_{PACK[key(e)]}.png")).convert("RGBA")
    k = max(1, size // art.width)
    art = art.resize((art.width * k, art.height * k), Image.NEAREST)
    cell = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    ox, oy = (art.width - size) // 2, (art.height - size) // 2
    if ox >= 0:
        art = art.crop((ox, max(oy, 0), ox + size, max(oy, 0) + min(size, art.height)))
    cell.alpha_composite(art, (max(0, -ox), max(0, -oy)))
    return cell


def _rgb555(r, g, b, a):
    if a < 128:
        return 0                                            # the transparent sentinel
    return ((r >> 3) << 10 | (g >> 3) << 5 | (b >> 3)) or 1  # opaque black -> 0x0001 (~black): not read as transparent


def frames_for(e, size):
    """([RGBA cell], ms per frame; 0 for a still) of an emoji of the set."""
    c = _custom_by_key().get(key(e))
    if c:
        return custom_frames(os.path.join(CUSTOM_DIR, c["file"]), size)
    return [made_for(e, size)], 0


def build(emojis=None, size=SIZE):
    """(emoji.bin bytes, map bytes, emoji list) for the firmware's content, with size x size cells (an animated
    emoji has several). Colours are kept at the panel's RGB555 precision; more than 255 are reduced to 255."""
    emojis = available(emojis, size)
    anims = [frames_for(e, size) for e in emojis]
    pics = [f for fr, _ in anims for f in fr]
    colours = Counter(_rgb555(*p) for c in pics for p in c.getdata())
    colours.pop(0, None)
    if len(colours) > 255:                              # reduce: quantize the whole set together
        strip = Image.new("RGBA", (size, size * len(pics)))
        for n, c in enumerate(pics):
            strip.paste(c, (0, n * size))
        q = strip.convert("RGB").quantize(255, dither=Image.Dither.NONE).convert("RGB")
        alpha = strip.getchannel("A")
        strip = Image.merge("RGBA", (*q.split(), alpha))
        pics = [strip.crop((0, n * size, size, (n + 1) * size)) for n in range(len(pics))]
        colours = Counter(_rgb555(*p) for c in pics for p in c.getdata())
        colours.pop(0, None)
    palette = [0] + [c for c, _ in colours.most_common()]
    index = {c: i for i, c in enumerate(palette)}
    m = "".join(e + "\n" for e in emojis).encode("utf-8")
    table, first = b"", 0
    for fr, ms in anims:
        table += struct.pack(">HHH", first, len(fr), ms)
        first += len(fr)
    px = bytes(index[_rgb555(*p)] if _rgb555(*p) else 0 for c in pics for p in c.getdata())
    raw = struct.pack(f">{len(palette)}H", *palette) + m + table + px
    data = struct.pack(HEADER, MAGIC, size, len(emojis), len(palette), len(pics), len(m), len(raw)) + \
        zlib.compress(raw, 9)
    return data, m, emojis


def write(out_dir, emojis=None, size=SIZE):
    """Writes emoji.bin into out_dir; returns the emoji list."""
    data, _, emojis = build(emojis, size)
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "emoji.bin"), "wb") as f:
        f.write(data)
    return emojis


def update(sizes=(SIZE,)):
    """Rebuilds the committed firmware/emoji/<size>/emoji.bin from the emoji pack (needs the pack here)."""
    if not PACK:
        raise SystemExit(f"no emoji pack in {paths.EMOJI_PACK}")
    for size in sizes:
        print(size, len(write(os.path.join(EMOJI_DIR, str(size)), size=size)), "emoji")


def encode(text, emojis=None):
    """The bytes to send to firmware with native emoji: every emoji of the set as UTF-8 (longest match first,
    U+FE0F dropped), everything else Latin-1 (the in-text effect commands, 0x1F ..., are plain Latin-1) ('?' when it has no
    Latin-1 form)."""
    keys = sorted(set(available(emojis)), key=len, reverse=True)
    out, i = bytearray(), 0
    while i < len(text):
        if text[i] == "\uFE0F":
            i += 1
            continue
        rest = key(text[i:i + 12])
        hit = next((k for k in keys if rest.startswith(k)), None)
        if hit:
            out += hit.encode("utf-8")
            n, j = 0, i                                  # advance past the emoji's code points in the text
            while n < len(hit):
                if text[j] != "\uFE0F":
                    n += 1
                j += 1
            i = j
            continue
        out += text[i].encode("latin-1", "replace")
        i += 1
    return bytes(out)


# ---- the firmware's drawing rules, for the studio's preview (mirrors firmware/ext/native_emoji.c) ----
_sheet_cache = {}


def _sheet(emojis=None, size=SIZE):
    """(keys as UTF-8 bytes in cell order, [cell pixels as RGB555 or None, frame 0]) as the firmware unpacks
    them."""
    k = (None if emojis is None else tuple(emojis), size, _custom_stamp())
    if k not in _sheet_cache:
        data, _, _ = build(emojis, size)
        s, palette, keys, px = unpack(data)
        _sheet_cache[k] = ([e.encode("utf-8") for e in keys], [[palette[v] if v else None for v in c] for c in px])
    return _sheet_cache[k]


def parse(data, emojis=None, size=SIZE):
    """The glyphs of text bytes as the firmware reads them: ("c", byte) or ("e", cell). An emoji is the longest
    map entry starting at a byte >= 0xC2, plus a following U+FE0F if present."""
    keys, _ = _sheet(emojis, size)
    out, i = [], 0
    while i < len(data):
        best, cell = 0, None
        if data[i] >= 0xC2:
            for n, k in enumerate(keys):
                if len(k) > best and data.startswith(k, i):
                    best, cell = len(k), n
            if best and data[i + best:i + best + 3] == b"\xef\xb8\x8f":
                best += 3
        if best:
            out.append(("e", cell))
            i += best
        else:
            out.append(("c", data[i]))
            i += 1
    return out


def layout(ft, glyphs, size=SIZE):
    """[(glyph, x, width)] and the total width, as ext_text_width / the drawers place them (an emoji is a
    proportional glyph as wide as a cell)."""
    out, x, prev = [], 0, 1
    for g in glyphs:
        w, p = (size, 1) if g[0] == "e" else (ft.width[g[1]], ft.prop[g[1]])
        if prev == 0 and p:
            x += 1
        out.append((g, x, w))
        x += w + (1 if p else 0)
        prev = p
    return out, x


def top(h, size=SIZE):
    """Where the emoji's top row goes relative to an h-pixel text line (emoji_draw: centred, C division, which
    rounds toward zero)."""
    return int((h - size) / 2)


def pixels(cell, h, emojis=None, size=SIZE):
    """[(dx, dy, RGB555)] of an emoji's opaque pixels on an h-pixel text line, drawn 1:1 (emoji_draw's rule); dy can
    be negative, or past h, when the emoji is taller than the font."""
    _, cells = _sheet(emojis, size)
    t = top(h, size)
    return [(n % size, t + n // size, v) for n, v in enumerate(cells[cell]) if v is not None]


if __name__ == "__main__":
    if sys.argv[1:2] == ["update"]:
        update(tuple(int(a) for a in sys.argv[2:]) or (SIZE,))
