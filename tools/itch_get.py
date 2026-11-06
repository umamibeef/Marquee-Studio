#!/usr/bin/env python3
"""Download named files from a free itch.io asset page (used for monogram and m5x7, see THIRD-PARTY.md).
Usage: itch_get.py USER GAME OUTDIR FILE [FILE...]   e.g. itch_get.py managore m5x7 /tmp/m5x7 m5x7.ttf
Check the page's asset licence before vendoring anything."""
import http.cookiejar, json, re, sys, urllib.parse, urllib.request
user, game, out = sys.argv[1], sys.argv[2], sys.argv[3]
want = set(sys.argv[4:])
cj = http.cookiejar.CookieJar()
op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(cj))
op.addheaders = [("User-Agent", "Mozilla/5.0")]
base = f"https://{user}.itch.io/{game}"
def get(u, data=None):
    return op.open(u, urllib.parse.urlencode(data).encode() if data is not None else None).read()
csrf = lambda h: re.search(r'name="csrf_token" value="([^"]+)"', h).group(1)
page = get(base).decode()
dl = json.loads(get(base + "/download_url", {"csrf_token": csrf(page)}))["url"]
key = urllib.parse.unquote(dl.rsplit("/download/", 1)[1])
dpage = get(dl).decode()
for m in re.finditer(r'data-upload_id="(\d+)"', dpage):
    name = re.search(r'title="([^"]+)"', dpage[m.end():m.end() + 1500]).group(1)
    if name not in want: continue
    r = json.loads(get(f"{base}/file/{m.group(1)}?source=view_game&as_props=1&after_download_lightbox=true", {"csrf_token": csrf(page)}))
    if "url" not in r: print(name, r); continue
    data = get(r["url"]); open(f"{out}/{name}", "wb").write(data); print("got", name, len(data))
