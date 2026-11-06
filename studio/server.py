#!/usr/bin/env python3
"""Umami Marquee Studio — local web app to fully configure the sign.
Run: ../venv/bin/python server.py   then open http://127.0.0.1:8750
"""
import os, json, io, glob, re
from flask import Flask, request, jsonify, send_file, send_from_directory, abort
import core
from marquee_studio import paths

HERE = os.path.dirname(os.path.abspath(__file__))
CFG = os.environ.get("MARQUEE_STUDIO_CONFIG") or os.path.join(HERE, "config.json")
# The web studio (frontend/apps/studio, `npm run build`). Everything it previews it draws itself (@marquee/sign-emu);
# the server hands over files, keeps the settings, builds bytes and talks to serial ports.
DIST = os.path.join(os.path.dirname(HERE), "frontend", "apps", "studio", "dist")
app = Flask(__name__, static_folder=None)


def native(cfg=None):
    """The sign runs firmware with native emoji (Settings): emoji are sent and previewed as UTF-8 glyphs."""
    return bool((cfg or load_cfg()).get("native_emoji", True))


def emoji_size(cfg=None):
    """The native emoji sheet's cell size: the panel height (emoji are made for the display)."""
    return int((cfg or load_cfg())["panel"]["height"])


def load_cfg():
    if os.path.exists(CFG):
        with open(CFG) as f:
            return json.load(f)
    return core.DEFAULT_CONFIG


@app.route("/")
@app.route("/<path:path>")
def index(path=""):
    """The built web studio: its files, and index.html for every client-side route."""
    if path.startswith("api/"):
        abort(404)
    if not os.path.exists(os.path.join(DIST, "index.html")):
        return ("The studio is not built yet: cd frontend && npm install && npm run build", 503,
                {"Content-Type": "text/plain"})
    if path and os.path.isfile(os.path.join(DIST, path)):
        r = send_from_directory(DIST, path)
        if path.startswith("assets/"):                # content-hashed names
            r.headers["Cache-Control"] = "public, max-age=31536000, immutable"
        return r
    r = send_from_directory(DIST, "index.html")
    r.headers["Cache-Control"] = "no-store"
    return r


@app.route("/api/assets")
def assets():
    return jsonify(core.list_assets())


@app.route("/api/sign_files")
def sign_files():
    """What the web studio's own sign emulation (frontend/packages/sign-emu) loads: the sign's fonts in the firmware's
    load order (fonts_load: fonte00..fonte99), the fonts its automatic picks may choose (fonts.auto), and the effect
    animations by slot (effects_load_slots: effect00..99)."""
    from marquee_studio.fonts import sign_font_names, auto_fonts
    anims = os.path.join(core.CONTENT, "animations")
    effects = {int(n[6:8]): n for n in sorted(os.listdir(anims)) if re.fullmatch(r"effect\d\d\.flc", n)}
    return jsonify({"fonts": sign_font_names(), "effects": effects, "auto": sorted(auto_fonts())})


@app.route("/api/emoji_sheet")
def emoji_sheet():
    """The sign's native emoji sheet (emoji.bin, as a content install puts it on the sign) at the panel's emoji size,
    for the in-browser emulation; 404 when the sign is set up without native emoji."""
    if not native():
        abort(404)
    from marquee_studio import native_emoji
    data, _, _ = native_emoji.build(size=emoji_size())
    return send_file(io.BytesIO(data), mimetype="application/octet-stream")


@app.route("/api/content/<kind>/<name>")
def content_file(kind, name):
    """A font sheet or an animation, as stored (for the in-browser emulation)."""
    if kind not in ("fonts", "animations") or name != os.path.basename(name) or not re.fullmatch(r"[\w.-]+", name):
        abort(404)
    return send_from_directory(os.path.join(core.CONTENT, kind), name)


@app.route("/api/config", methods=["GET", "POST"])
def config():
    if request.method == "POST":
        with open(CFG, "w") as f:                      # closed (and flushed) before the next read
            json.dump(request.json, f, indent=2)
        return jsonify({"ok": True})
    return jsonify(load_cfg())


@app.route("/api/emoji")
def emoji_list():
    """The emoji picker's catalogue (core.emoji_catalog: the emoji pack's pictures when it is here, else the
    committed native emoji sheet's), with its name (from Python's Unicode database), a group, the picture's URL, and whether the
    sign's native emoji sheet carries it."""
    from marquee_studio import native_emoji
    on_sign = set(native_emoji.available())
    own = [{"e": c["e"], "cps": c["cp"], "name": c["name"], "group": "Custom", "file": f"custom/{c['cp']}.gif",
            "custom": True} for c in native_emoji.custom()]
    return jsonify([{**e, "url": "/emoji/" + e["file"], "native": native_emoji.key(e["e"]) in on_sign}
                    for e in core.emoji_catalog() + own])


@app.route("/emoji/custom/<cp>.gif")
def emoji_custom_image(cp):
    """An imported emoji as the sign shows it: its frames at the panel's cell size, at the sign's frame time."""
    from marquee_studio import native_emoji
    c = next((c for c in native_emoji.custom() if c["cp"] == cp.upper()), None)
    if not c:
        abort(404)
    frames, ms = native_emoji.custom_frames(os.path.join(native_emoji.CUSTOM_DIR, c["file"]), emoji_size())
    buf = io.BytesIO()
    frames[0].save(buf, "GIF", save_all=True, append_images=frames[1:], duration=ms or 1000, loop=0, disposal=2)
    return send_file(io.BytesIO(buf.getvalue()), mimetype="image/gif")


@app.route("/api/emoji/import", methods=["POST"])
def emoji_import():
    """Import a GIF (or PNG, APNG, WebP) as an emoji of the sign's set, with the next private-use code point
    (U+E000 up). It goes to the sign with the apps (the store's emoji.bin) and shows in every preview."""
    from marquee_studio import native_emoji
    f = request.files.get("file")
    if not f:
        return jsonify({"ok": False, "error": "no file"}), 400
    try:
        e = native_emoji.add_custom(f.read(), request.form.get("name", ""))
    except ValueError as err:
        return jsonify({"ok": False, "error": str(err)}), 400
    return jsonify({"ok": True, "e": e["e"], "cps": e["cp"], "name": e["name"]})


@app.route("/api/emoji/custom/<cp>", methods=["DELETE"])
def emoji_remove(cp):
    """Remove an imported emoji (texts that use it then show nothing there)."""
    from marquee_studio import native_emoji
    if not re.fullmatch(r"[0-9A-Fa-f]{4,5}", cp):
        abort(404)
    return jsonify({"ok": native_emoji.remove_custom(cp)})


@app.route("/emoji/pack/<int:n>.png")
def emoji_pack_image(n):
    """A picture of the emoji pack (local only: the pack is not committed)."""
    r = send_from_directory(os.path.join(paths.EMOJI_PACK, "Emojis_Flat_16x16"), f"Emojis_Flat_16x16_{n}.png")
    r.headers["Cache-Control"] = "public, max-age=86400"
    return r


@app.route("/emoji/sheet/<int:n>.png")
def emoji_sheet_image(n):
    """A picture of the committed native emoji sheet (firmware/emoji/<size>/emoji.bin): the catalogue without the
    emoji pack."""
    png = core.sheet_emoji_png(n)
    if png is None:
        abort(404)
    r = send_file(io.BytesIO(png), mimetype="image/png")
    r.headers["Cache-Control"] = "public, max-age=86400"
    return r


@app.route("/api/import_animation", methods=["POST"])
def import_animation():
    """Upload a GIF (or APNG/WebP) and convert it to an FLC sized for the panel."""
    import tempfile
    f = request.files["file"]
    cfg = load_cfg()
    W, H = cfg["panel"]["width"], cfg["panel"]["height"]
    depth = 8 if request.form.get("depth") == "8" else 16
    fit = request.form.get("fit", "contain")
    if fit not in ("contain", "cover", "stretch"):
        fit = "contain"
    with tempfile.NamedTemporaryFile(suffix=os.path.splitext(f.filename or "")[1]) as tmp:
        f.save(tmp.name)
        try:
            info = core.import_animation(tmp.name, request.form.get("name") or f.filename, W, H, depth, fit)
        except Exception as e:
            return jsonify({"ok": False, "error": f"could not convert: {e}"}), 400
    return jsonify({"ok": True, **info})


@app.route("/api/upload_asset", methods=["POST"])
def upload_asset():
    """Drop a file into the content folder: an .flc into animations/ (checked: it must decode), or a font PNG
    into fonts/ (a 16 x 16 glyph grid; named fonteNN.png, the only names the sign loads)."""
    kind = request.form.get("kind", "animations")
    f = request.files["file"]
    data = f.read()
    if kind == "animations":
        name = core.safe_asset_name(f.filename, ".flc")      # never trust the browser's file name
        try:
            core.FLIC(data).frames_rgb()
        except Exception as e:
            return jsonify({"ok": False, "error": f"not a FLIC animation the sign can play: {e}"}), 400
        dest = os.path.join(core.CONTENT, "animations", name)
    else:
        from PIL import Image
        try:
            w, h = Image.open(io.BytesIO(data)).size
        except Exception:
            return jsonify({"ok": False, "error": "not a PNG image"}), 400
        if w % 16 or h % 16 or w < 64 or h < 64:
            return jsonify({"ok": False, "error": f"a sign font is a 16 x 16 grid of glyph cells; {w} x {h} is not"}), 400
        name = core.upload_font_name(f.filename)
        dest = os.path.join(core.CONTENT, "fonts", name)
    open(dest, "wb").write(data)
    return jsonify({"ok": True, "name": name})


@app.route("/api/pack", methods=["POST"])
def pack():
    from marquee_studio import lfs
    cfg = load_cfg()
    sel = request.json.get("files", [])
    files = []
    # include a text config/theme
    theme = render_theme(cfg)
    files.append(("theme", theme.encode()))
    for name in sel:
        for sub in ("animations", "fonts"):
            p = os.path.join(core.CONTENT, sub, name)
            if os.path.exists(p):
                files.append((name, open(p, "rb").read())); break
    if native(cfg):                                       # the sheet and map for firmware with native emoji
        from marquee_studio import native_emoji
        data, _, _ = native_emoji.build(size=emoji_size(cfg))
        files += [("emoji.bin", data)]
    img = lfs.pack(files, compress=True)
    buf = io.BytesIO(img); buf.seek(0)
    return send_file(buf, mimetype="application/octet-stream",
                     as_attachment=True, download_name="content.lfs")


def render_theme(cfg):
    lines = ["[GROUP]"]
    lines.append(f"Download,Mode={cfg['PROTOCOL'].get('Download Mode','direct')}")
    lines.append(f"Download,Datarate={cfg['PROTOCOL'].get('Download Datarate',115200)}")
    lines.append(f"Protocol={cfg['PROTOCOL'].get('Protocol','Cougar')}")
    for i, lv in enumerate(cfg.get("levels", [])):
        g = cfg['PROTOCOL'].get('Protocol', 'Cougar').split()[0]
        for k in ("Address", "Jackpot Level", "Text Rate", "Flash On", "Flash Off"):
            if k in lv:
                lines.append(f"{g},Level,{i},{k}={lv[k]}")
    return "\r\n".join(lines) + "\r\n"


@app.route("/api/import_font", methods=["POST"])
def import_font():
    """Import a vendored pixel font (third_party/fonts, THIRD-PARTY.md) as a new fonteNN.png."""
    try:
        name, info = core.import_font(request.json.get("path", ""), request.json.get("upper", "latin1"))
    except Exception as e:
        return jsonify({"ok": False, "error": str(e)}), 400
    return jsonify({"ok": True, "name": name, **info})


def markup_bytes(req, H):
    """The message line bytes of a request: {"segments": [...]} (encoded) or {"raw": "1b 30 ..."} (as is)."""
    if req.get("raw") is not None:
        return bytes.fromhex(req["raw"].replace(" ", "").replace("\n", ""))
    return core.encode_markup(req.get("segments", []), H, native=native())


@app.route("/api/markup", methods=["POST"])
def markup():
    """Encode (or take raw) message markup: bytes, how the firmware reads them back, warnings, the RS-485
    frame it would make. Builds bytes only; nothing is sent. (The preview is the web studio's own emulation.)"""
    from marquee_studio import signproto as sp
    cfg = load_cfg()
    W = cfg["panel"]["width"]; H = cfg["panel"]["height"]
    r24 = int(request.json.get("r24", 0))
    try:
        data = markup_bytes(request.json, H)
    except ValueError as e:
        return jsonify({"ok": False, "error": str(e)})
    out = {"ok": True, "bytes": data.hex(" "), "length": len(data),
           "segments": core.decode_markup(data, H, r24=r24, W=W), "warnings": core.markup_warnings(data, W, H, r24=r24)}
    try:
        out["frame"] = sp.rs485_markup_frame(data, level=int(request.json.get("level", 0))).hex(" ")
    except ValueError as e:
        out["frame_error"] = str(e)
    scrolls, width, font = core.record_text_scrolls(data, W, H)
    out["record"] = {"scrolls": scrolls, "width": width, "font": font, "window": [W, H]}
    return jsonify(out)


# A running umami-marquee-sim exposes the simulated sign's ports as devices and links them here (simulator/src/pty_bridge.cpp):
# the studio sends to the simulator exactly as to the real sign's USB serial adapter.
SIM_PORTS = [(os.path.join(os.path.dirname(HERE), "build", "sim", "rs485"), "Simulator: RS-485 (umami-marquee-sim)"),
             (os.path.join(os.path.dirname(HERE), "build", "sim", "db9"), "Simulator: DB9 (umami-marquee-sim)")]


@app.route("/api/serial_ports")
def serial_ports():
    from marquee_studio import sign_window
    if sign_window.TCP:                                  # a container: it can't see /dev; ask the host's Sign window
        try:
            ports = sign_window.ports()
        except Exception:
            ports = []
        labels = {p: ("Simulator: " + os.path.basename(p).upper() + " (umami-marquee-sim)")
                  for p in ports if "/build/sim/" in p}
        return jsonify({"ports": ports, "labels": labels})
    sim = [(p, label) for p, label in SIM_PORTS if os.path.exists(p)]   # a dangling link: no simulator running
    ports = [p for p, _ in sim] + sorted(glob.glob("/dev/cu.*"))
    return jsonify({"ports": ports, "labels": dict(sim)})


@app.route("/api/apps")
def apps_catalogue():
    """The programmable apps in apps/ (docs/APPS.md): {name: app.json}."""
    from marquee_studio import store_upload
    return jsonify(store_upload.available())


@app.route("/api/app_bundle", methods=["POST"])
def app_bundle():
    """One app instance's bundle (bytes, built here: clang + tools/mkapp.py) for the web Studio's own preview."""
    from marquee_studio import store_upload
    try:
        data = store_upload.instance(request.json["app"], request.json.get("settings") or {})
    except Exception as e:
        return jsonify({"ok": False, "error": str(e)}), 400
    return send_file(io.BytesIO(data), mimetype="application/octet-stream")


@app.route("/api/feed/<name>", methods=["POST"])
def feed_values(name):
    """A data feed's current values for an app's settings (the preview shows what the sign would get)."""
    from marquee_studio import feeds
    if name not in feeds.FEEDS:
        abort(404)
    try:
        return jsonify({"ok": True, "values": feeds.fetch(name, request.json or {})})
    except Exception as e:
        return jsonify({"ok": False, "error": str(e)})


# the feeders: after an upload, each data app's feed is fetched on its schedule and sent to that port
import threading
_FEED_LOCK = threading.Lock()
_FEEDERS = {}


def _feed_cmds(placements, manifests):
    from marquee_studio import feeds, store_upload
    cmds = []
    for p in placements:
        feed = manifests.get(p["app"], {}).get("feed")
        if feed:
            settings = {s["key"]: s.get("default") for s in manifests[p["app"]].get("settings", [])}
            settings.update(p.get("settings") or {})
            cmds += store_upload.data_commands(p["line"], feeds.fetch(feed, settings))
    return cmds


def _feed_once(port, placements, manifests):
    """the data apps' current values to the sign, through the Sign window (its Content tab's log)"""
    from marquee_studio import sign_window
    cmds = _feed_cmds(placements, manifests)
    if cmds:
        with _FEED_LOCK:
            sign_window.upload(port, {}, cmds, feed=True)
    return cmds


def _feeder(port, placements, manifests, stop):
    from marquee_studio import feeds
    every = min([feeds.FEEDS[m["feed"]] for m in manifests.values() if m.get("feed")] or [600])
    while not stop.wait(every):
        try:
            _feed_once(port, placements, manifests)
        except Exception:
            pass                                       # the sign is away: try again next time


@app.route("/api/upload_apps", methods=["POST"])
def upload_apps():
    """Install the Apps page's apps on the sign at a serial port (or the simulator's build/sim/db9), through
    umami-marquee-sim's Sign window (marquee_studio/sign_window.py: the upload shows there): each placement's bundle
    into the content store as app<line>.mqap (and emoji.bin with imported emoji), the sign's time, and the data apps'
    current values; then keep feeding those while this server runs. Returns the sign's own listing."""
    from marquee_studio import sign_window, store_upload
    port = request.json.get("port", "")
    placements = [p for p in request.json.get("placements", []) if p.get("enabled", True)]
    try:
        manifests = store_upload.available()
        files = store_upload.apps_files(placements, emoji_size())
        fed = _feed_cmds(placements, manifests)
        with _FEED_LOCK:
            res = sign_window.upload(port, files, [f"time {store_upload.sign_seconds():x}", *fed, "apps"])
    except Exception as e:
        return jsonify({"ok": False, "error": str(e)})
    old = _FEEDERS.pop(port, None)
    if old:
        old.set()
    if fed:
        stop = threading.Event()
        _FEEDERS[port] = stop
        threading.Thread(target=_feeder, args=(port, placements, manifests, stop), daemon=True).start()
    return jsonify({"ok": True, "apps": len(files), "bytes": sum(len(b) for b in files.values()),
                    "listing": res["replies"].get("apps", []), "fed": len(fed)})

@app.route("/api/send_rs485", methods=["POST"])
def send_rs485():
    """Push one text frame over an RS-485 adapter (live display update)."""
    from marquee_studio import signproto as sp
    port = request.json["port"]; text = request.json.get("text", "")
    nat = native()
    level = int(request.json.get("level", 0)); baud = int(request.json.get("baud", 19200))
    if "segments" in request.json or "raw" in request.json:     # message markup: the exact bytes
        try:
            frame = sp.rs485_markup_frame(markup_bytes(request.json, load_cfg()["panel"]["height"]), level=level)
        except ValueError as e:
            return jsonify({"ok": False, "error": str(e)})
    elif nat:                                  # native emoji: the emoji go as UTF-8 (native_emoji.encode)
        from marquee_studio import native_emoji
        try:
            frame = sp.rs485_frame(1, sp.rs485_markup_record(native_emoji.encode(text), level=level))
        except ValueError as e:
            return jsonify({"ok": False, "error": str(e)})
    else:
        frame = sp.rs485_text_frame(text, level=level)
    from marquee_studio import sign_window                   # through the Sign window (visible there), never the port here
    try:
        sign_window.send_rs485(port, baud, frame)
        return jsonify({"ok": True, "sent": frame.hex(" ")})
    except Exception as e:
        return jsonify({"ok": False, "error": str(e), "frame": frame.hex(" ")})


if __name__ == "__main__":
    port = int(os.environ.get("MARQUEE_STUDIO_PORT", "8750"))
    host = os.environ.get("MARQUEE_STUDIO_HOST", "127.0.0.1")   # 0.0.0.0 in the container
    debug = os.environ.get("MARQUEE_STUDIO_DEBUG") == "1"       # Flask reloader for dev
    print(f"Umami Marquee Studio -> http://{host}:{port}")
    app.run(host=host, port=port, debug=debug)
