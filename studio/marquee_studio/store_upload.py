"""Apps and content for the sign (docs/APPS.md): app instances built from apps/<name> with their settings
(tools/mkapp.py), the content store's console commands (firmware/ext/store.c, for the simulator's consoledump), the
sign's time and the values data apps get (adata). Sending them to a sign is sign_window.py's: through
umami-marquee-sim's Sign window, never straight to a serial port."""
import os, sys, time

from . import paths

APPS_DIR = os.path.join(paths.REPO, "apps")
CHUNK = 30                                      # bytes per "sd" line (the console's lines are 64 characters)
RAW = 256                                       # bytes per "sraw" piece


def _mkapp():
    sys.path.insert(0, os.path.join(paths.REPO, "tools"))
    import mkapp
    return mkapp


def available():
    """the apps in apps/: {name: manifest}"""
    import json
    out = {}
    for n in sorted(os.listdir(APPS_DIR)):
        m = os.path.join(APPS_DIR, n, "app.json")
        if os.path.isfile(m) and os.path.isfile(os.path.join(APPS_DIR, n, "app.c")):
            out[n] = json.load(open(m, encoding="utf-8"))
    return out


def instance(app, settings=None):
    """the bundle of app (a name in apps/, or an app's folder) with these settings over its defaults; the setting
    background_anim (an animation of the studio's content folder) is bundled as the asset "background", which show.h
    plays behind the text"""
    settings = {k: v for k, v in (settings or {}).items() if v is not None}
    extra = {}
    bg = settings.get("background_anim")
    if bg:
        p = os.path.join(paths.CONTENT, "animations", os.path.basename(str(bg)))
        if not os.path.isfile(p):
            raise ValueError(f"no animation {bg} in the content folder")
        extra["background"] = p
    return _mkapp().build(os.path.join(APPS_DIR, app), settings, extra_assets=extra)


def sign_seconds(unix=None):
    """local wall-clock seconds since 1970 (the sign has no time zone)"""
    import datetime
    t = time.time() if unix is None else unix
    return int(t) + int(datetime.datetime.fromtimestamp(t).astimezone().utcoffset().total_seconds())


def store_commands(files):
    """console commands that replace the content store with files ({name: bytes}): each file's data as raw
    transfers of RAW bytes (("raw", bytes): "sraw N", then the bytes), each acknowledged before the next"""
    cmds = ["sbegin"]
    for name, data in files.items():
        cmds.append(f"sfile {name} {len(data):x}")
        for k in range(0, len(data), RAW):                 # pieces the sign acknowledges: its receive buffer
            cmds.append(("raw", bytes(data[k:k + RAW])))   # must not overrun (a whole file at once does)
    cmds.append("scommit")
    return cmds


def as_text(cmds):
    """the commands as console lines (a raw transfer as "sraw HEX", as the simulator's consoledump replays it)"""
    return [f"sraw {c[1].hex()}" if isinstance(c, tuple) else c for c in cmds]


def data_commands(line, values):
    """console lines that send values ({key: text}) to the app on line"""
    from . import native_emoji
    out = []
    for k, v in values.items():
        b = native_emoji.encode(str(v))[:28]
        out.append(f"adata {line:x} {k} {b.hex()}")
    return out


def apps_files(placements, emoji_size=16):
    """{"app<line>.mqap": bundle} for [{"app", "line", "settings"}], and emoji.bin: the sign's whole set
    (firmware/emoji/set.txt and the emoji imported into the studio). The firmware reads emoji.bin from the store before
    the boot flash's (firmware/ext/native_emoji.c), so the sign shows the set the Studio previews, whatever set its
    installed firmware was built with."""
    from . import native_emoji
    files = {f"app{p['line']}.mqap": instance(p["app"], p.get("settings")) for p in placements}
    files["emoji.bin"] = native_emoji.build(size=emoji_size)[0]
    return files
