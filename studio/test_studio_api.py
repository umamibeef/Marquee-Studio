#!/usr/bin/env python3
"""The studio web API the React front end uses (server.py), on a temporary copy of the content folder.
Checks that every animation and font in the folders is listed and usable, the new preview endpoints, drop-in
uploads, and native emoji (the catalogue and its pictures). Never opens a serial port.
Run: ../venv/bin/python test_studio_api.py"""
import base64
import io
import os
import re
import shutil
import sys
import json
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "content")
TMP = tempfile.mkdtemp(prefix="marquee-content-")
os.environ["MARQUEE_STUDIO_CONTENT"] = TMP          # before importing the studio
CFG_TMP = os.path.join(TMP, "config.json")          # the studio's settings: a copy, never the real one
if os.path.exists(os.path.join(HERE, "config.json")):
    shutil.copy(os.path.join(HERE, "config.json"), CFG_TMP)
os.environ["MARQUEE_STUDIO_CONFIG"] = CFG_TMP
sys.path.insert(0, HERE)

fails = 0


def check(cond, what):
    global fails
    print(("  ok   " if cond else "  FAIL ") + what)
    fails += not cond


def main():
    if not os.path.isdir(os.path.join(SRC, "animations")) or not os.path.isdir(os.path.join(SRC, "fonts")):
        print("SKIP: studio/content has no animations/fonts (extract them from the sign first)")
        return
    for sub in ("animations", "fonts"):
        shutil.copytree(os.path.join(SRC, sub), os.path.join(TMP, sub))
    # an extra animation that is not an effect slot, with an upper-case extension
    first = sorted(f for f in os.listdir(os.path.join(TMP, "animations")) if f.endswith(".flc"))[0]
    shutil.copy(os.path.join(TMP, "animations", first), os.path.join(TMP, "animations", "Sparkle.FLC"))

    import server
    with open(CFG_TMP, "w") as f:                        # fixed settings for the test, in the temporary copy
        json.dump({"panel": {"width": 80, "height": 16}, "PROTOCOL": {}, "levels": []}, f)
    c = server.app.test_client()

    a = c.get("/api/assets").get_json()
    on_disk = sorted(f for f in os.listdir(os.path.join(TMP, "animations")) if f.lower().endswith(".flc"))
    check(a["animations"] == on_disk, f"every animation in the folder is listed ({len(on_disk)})")
    check(set(a["animation_info"]) == set(on_disk), "animation_info for each")
    check(a["animation_info"]["Sparkle.FLC"]["slot"] is None, "a non-effect animation has no slot")
    d = os.listdir(os.path.join(TMP, "fonts"))                # our firmware's order: the sign's own, then MQF by name
    from marquee_studio.fonts import stock_kept
    dropped = {f"fonte{n:02d}.png" for n in range(1, 9)} - stock_kept()
    fonts = sorted(f for f in d if re.fullmatch(r"fonte\d\d\.png", f) and f not in dropped) + \
        sorted(f for f in d if f.endswith(".mqf"))
    check(a["fonts"] == fonts, f"every font in the folder is listed ({len(fonts)})")
    check(all("cell" in a["font_info"][f] for f in fonts), "font_info has each font's cell size")

    # previews are the web studio's own emulation (@marquee/sign-emu): the server renders none
    for route in ("/api/effects_gallery", "/api/effect_preview?name=x.flc", "/api/font_sample?font=fonte04.png"):
        check(c.get(route).status_code in (404, 405), f"no server preview: {route}")
    for route in ("/api/preview", "/api/anim_preview"):
        check(c.post(route, json={}).status_code in (404, 405), f"no server preview: {route}")
    mk = c.post("/api/markup", json={"segments": [{"text": "HI", "font": None}], "preview": True}).get_json()
    check(mk.get("ok") and "preview" not in mk, "markup builds bytes only")
    sf = c.get("/api/sign_files").get_json()
    check(sf["fonts"] == fonts, "sign_files: the sign's fonts in load order")
    check(c.get("/api/content/fonts/" + sf["fonts"][0]).status_code == 200, "the emulation gets the font sheets as stored")

    em = c.get("/api/emoji").get_json()
    check(len(em) > 100 and all(re.fullmatch(r"/emoji/(pack|sheet)/\d+\.png", e["url"]) for e in em),
          f"emoji catalogue ({len(em)}, from the {'emoji pack' if '/pack/' in em[0]['url'] else 'committed sheet'})")
    check(any(e["e"] == "🍒" and "cherr" in e["name"] for e in em), "emoji have names")
    r = c.get(em[0]["url"])
    check(r.status_code == 200 and r.mimetype == "image/png", f"emoji pictures are served ({em[0]['url']})")
    bad = c.get("/emoji/..%2Fserver.py")
    check(b"send_rs485" not in bad.data, "the emoji route serves only emoji pictures")
    check(all(c.get(u).mimetype != "image/png" for u in ("/emoji/U+1F352.png", "/emoji/sheet/99999.png",
                                                          "/emoji/pack/99999999.png")),
          "emoji routes serve only the catalogue's pictures")

    # drop-in uploads
    flc = open(os.path.join(TMP, "animations", first), "rb").read()
    r = c.post("/api/upload_asset", data={"kind": "animations", "file": (io.BytesIO(flc), "my anim.flc")})
    check(r.get_json().get("name") == "my_anim.flc", "an .flc upload lands in animations/")
    r = c.post("/api/upload_asset", data={"kind": "animations", "file": (io.BytesIO(b"nope"), "x.flc")})
    check(r.status_code == 400, "a broken .flc is refused")
    png = open(os.path.join(TMP, "fonts", "fonte04.png"), "rb").read()
    r = c.post("/api/upload_asset", data={"kind": "fonts", "file": (io.BytesIO(png), "My Font.png")})
    name = r.get_json().get("name", "")
    check(name.startswith("fonte") and name in c.get("/api/assets").get_json()["fonts"],
          f"a font PNG upload becomes a sign font name ({name})")

    check(c.post("/api/gif", json={}).status_code in (404, 405), "GIF export is the web studio's own (no server rendering)")

    # native emoji (firmware/ext, marquee_studio/native_emoji.py)
    cfg = c.get("/api/config").json
    c.post("/api/config", json={**cfg, "native_emoji": True})
    check(c.get("/api/config").json.get("native_emoji") is True, "native emoji: the setting is saved")
    sheet = c.get("/api/emoji_sheet")
    check(sheet.status_code == 200 and sheet.data[:4] == b"EMJ2", "native emoji: the emulation gets the emoji sheet")
    # an imported, animated emoji (native_emoji.add_custom): the next private-use code point, in the catalogue, the
    # sheet (several frames) and the upload's files; then removed
    import io as _io
    from PIL import Image as _Image
    from marquee_studio import native_emoji as _ne, store_upload as _su
    fr = [_Image.new("RGBA", (16, 16), (255 * (k % 2), 200, 0, 255)) for k in range(3)]
    gif = _io.BytesIO()
    fr[0].save(gif, "GIF", save_all=True, append_images=fr[1:], duration=100, loop=0)
    r = c.post("/api/emoji/import", data={"file": (_io.BytesIO(gif.getvalue()), "spin.gif"), "name": "spin"},
               content_type="multipart/form-data").json
    check(r.get("ok") and r["cps"] == "E000", f"imported emoji: a private-use code point ({r})")
    own = [e for e in c.get("/api/emoji").json if e.get("custom")]
    check(len(own) == 1 and own[0]["native"] and own[0]["group"] == "Custom", "imported emoji: in the catalogue")
    pv = c.get(own[0]["url"])
    check(pv.status_code == 200 and _Image.open(_io.BytesIO(pv.data)).n_frames == 3, "imported emoji: animated preview")
    _, _, emojis, cells = _ne.unpack_frames(c.get("/api/emoji_sheet").data)
    check(emojis[-1] == "\ue000" and len(cells[-1][1]) == 3 and cells[-1][0] == 100, "imported emoji: in the sheet")
    check("emoji.bin" in _su.apps_files([]) and _ne.encode("a\ue000") == b"a\xee\x80\x80",
          "imported emoji: sent with the apps, encoded as UTF-8")
    check(c.delete("/api/emoji/custom/E000").json["ok"] and not any(e.get("custom") for e in c.get("/api/emoji").json)
          and "\ue000" not in _ne.unpack(_su.apps_files([])["emoji.bin"])[2], "imported emoji: removed")
    em = c.get("/api/emoji").json
    check(sum(e["native"] for e in em) >= 150 and any(e["native"] and e["e"] == "🍒" for e in em),
          "native emoji: the catalogue marks the sign's emoji set")
    check(c.post("/api/emoji_font", json={}).status_code in (404, 405), "colour emoji fonts are gone")
    check(server.markup_bytes({"segments": [{"text": "WIN 🍒", "font": None}]}, 16).endswith(b"\xf0\x9f\x8d\x92"),
          "native emoji: a message line carries the emoji as UTF-8")
    pk = c.post("/api/pack", json={"files": []})
    from marquee_studio import lfs
    names = [n for n, _ in lfs.unpack(pk.data)] if hasattr(lfs, "unpack") else None
    check(pk.status_code == 200 and (names is None or "emoji.bin" in set(names)),
          "native emoji: the content pack carries emoji.bin")
    c.post("/api/config", json={**cfg, "native_emoji": False})

    check(c.get("/classic").data == c.get("/").data, "one studio: /classic is the web studio too (no old page)")
    check(c.get("/api/does-not-exist").status_code == 404, "unknown API paths are 404, not the app")


try:
    main()
finally:
    shutil.rmtree(TMP, ignore_errors=True)
print("ALL OK" if not fails else f"{fails} FAILED")
sys.exit(1 if fails else 0)
