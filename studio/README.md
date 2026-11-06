# Umami Marquee Studio

Umami Marquee Studio is a local web app for configuring the Mikohn/Neurizon RGB LED sign. It designs messages,
previews them exactly as the panel renders them, manages animations, edits the configuration, and builds and
deploys content.

## Run
    ./run.sh            # or: ../venv/bin/python server.py
Then open http://127.0.0.1:8750 in your browser. It needs flask, pillow, numpy and fonttools, installed in ../venv.

The page at `/` is the React studio in `frontend/apps/studio` (React + Vite + TypeScript, shadcn/ui only, vendored
in `frontend/packages/ui`; docs/PORTAL-PLAN.md §7b). Build it once, and after each front-end change:

    cd ../frontend && npm ci && npm run build      # writes frontend/apps/studio/dist, served at /

Without a build, `/` says so. Every preview in the studio is drawn in the browser by the sign's emulation
(`frontend/packages/sign-emu`); the server only hands over the content files, keeps the settings, builds bytes and
talks to serial ports (the real sign, or the simulator's `build/sim/db9` and `build/sim/rs485`). To work on the front end:

    cd ../frontend && npm run dev                  # http://127.0.0.1:5173, /api proxied to the Flask server
    npm run check                                  # tsc --noEmit, eslint, build
    node scripts/screenshots.mjs                   # Playwright screenshots into /tmp/studio-shots

The content folder is the one place for animations and fonts: every `.flc` in `content/animations/` and every
font PNG in `content/fonts/` shows in the studio. Drop files on the Library page (GIF / APNG / WebP are converted
to FLC; a font PNG is named `fonteNN.png`, the only names the sign loads), or copy them into the folders.

### API (server.py)
| Endpoint | What |
|---|---|
| `GET /api/assets` | every animation and font in the content folder, with `animation_info` (size, frames, effect slot or none) and `font_info` (origin, licence, cell size), the palette, models, ... |
| `POST /api/preview` | a message spec rendered to RGB555 frames (40 fps), plus flash warnings |
| `GET /api/effect_preview?name=&role=fg\|bg&text=&font=` | one animation as the letters' fill or the background (any animation; non-effect ones in a free slot) |
| `GET /api/font_sample?font=&text=` | sample text drawn in one font by the firmware-exact renderer |
| `GET /api/emoji`, `GET /emoji/pack/<id>.png`, `GET /emoji/sheet/<n>.png` | the emoji picker's catalogue (names from Python's Unicode database, groups by code point range, whether the sign's native emoji set has each one) and its pictures: the emoji pack's when it is on this machine (JOI3's Flat Pixel Art Emojis, bought, local only and not redistributed), else the committed native emoji sheet's |
| `POST /api/gif` | the message as an animated GIF |
| `POST /api/upload_asset`, `POST /api/import_animation`, `POST /api/import_font` | add files to the content folder |
| `POST /api/markup`, `POST /api/pack`, `GET/POST /api/config`, `GET /api/serial_ports`, `POST /api/send_rs485` | as before |

## Code layout: the `marquee_studio` package
All the studio's logic is a Python package, `studio/marquee_studio/`, one module per concern (installable:
`pip install -e studio`). The web server (`server.py`) and the tests are thin users of it.

| Module | What it does |
|---|---|
| `paths` | where the repository, the content folder (`$MARQUEE_STUDIO_CONTENT`, else `studio/content`) and vendored files are |
| `sign_emu` | the firmware-exact panel model: fonts (`FwFont`), text lines, effects, FLC decoding (was `emulator/sign_emu.py`) |
| `font_render` | the older `SignFont` renderer used by the emulator demos (was `emulator/font_render.py`) |
| `palette`, `config` | named colours; protocols, the default configuration, the firmware's display models |
| `fonts` | the sign's fonts as the firmware measures them |
| `effects` | effect words (FG<<16 \| BG), effect slots, outline styles and colours |
| `render` | the message renderer (`render_message555`) and frame packing |
| `markup` | message-line markup: encode, decode, warnings, rendering |
| `amount`, `flash` | the sign's money formatter; Flash On/Off in milliseconds and the photosensitivity rule |
| `content` | the content folder: assets, animation and font imports, the emoji catalogue, what the UI lists |
| `font_import` | vendored pixel fonts |
| `native_emoji` | native emoji: the emoji.bin sheet, UTF-8 encoding, the firmware's drawing rules (the only emoji path) |
| `diag_patterns` | approximations of the sign's DIAGNOSTICS test patterns (was `studio/effects.py`) |
| `signproto`, `lfs`, `gif2flc` | sign protocol frames and CRCs; the LFS archive; GIF -> FLC (were in `tools/`) |

`studio/core.py` re-exports everything for older code. The old files (`emulator/sign_emu.py`, `tools/lfs.py`,
`tools/gif2flc.py`, ...) are aliases of the package modules, so existing imports and
command lines keep working. `tools/signproto.py` is the read-only DB9 probe tool, built on
`marquee_studio.signproto`.

## Tabs
- Design: compose a message (text, font, colour + outline from the sign's palette, scroll/static,
  text rate, flash on/off, optional background FLC animation, optional value). Live panel preview (the exact
  RGB555 render at the panel size you pick; 80×16 by default). "Send live (RS-485)" pushes the text to the sign through a USB-RS485 adapter.
- Message lines: compose a marked-up message line (docs/MESSAGE-MARKUP.md): segments with text, colours,
  entry/exit direction and speed, hold, font or centred. Live preview from the firmware's own text line, ported in
  `marquee_studio/sign_emu.py` (`TextLine`), a raw-bytes view (edit and decode), firmware warnings, and the RS-485 frame.
  The size picker on the Design tab also offers the firmware's 26 display models (AD0201 = the firmware's fallback and the studio's default; the real sign's model is not confirmed).
- Animations: browse and preview the extracted Autodesk FLIC animations on the panel; upload your own .flc.
- Configuration: panel size, wire protocol (the 10 the firmware supports), RS-485 baud, download settings,
  and per-level jackpot settings (address, level, text rate, flash). Saved to config.json.
- Build & Deploy: pack selected animations/fonts + the config into a `content.lfs` image for download to
  the sign (DB9, CRC-32 verified); pick a serial port.

## How it maps to the hardware (see ../SPEC.md)
- Panel: 16-bit RGB555, modular geometry. Animations: standard FLIC. Fonts: PNG glyph atlases.
- Content container: LFS archive (`marquee_studio/lfs.py`). Config: text key=value groups.
- Live updates: RS-485 records (`marquee_studio/signproto.py`). DB9: download-only (writes flash).

## Status
Rendering (panel, FLIC, fonts) is validated against the sign's own content. Serial send and LFS upload still need
end-to-end confirmation on the hardware (the adapter, and the unit's NVRAM config: selected protocol, level ids,
baud).

## Tests
    ../venv/bin/python test_markup.py   # markup encoder/decoder + emulator against ELITE_RGB under Unicorn
    ../venv/bin/python test_effects.py  # text effects (FG/BG effect words, outline) against ELITE_RGB under Unicorn
    ../venv/bin/python test_font_import.py  # imported pixel fonts; font_build under Unicorn
    ../venv/bin/python test_amount.py   # money formatter against the firmware's amount_format
    ../venv/bin/python test_studio_api.py  # the web API, on a temporary copy of the content folder

`test_markup.py` runs the real `textline_init` / `textline_step` / `message_text_next_token` and `font_build` on the
studio fonts and checks the parsed fields and every frame's pixels against the emulator (several display models, plus
the quirks). `test_effects.py` loads the real effectNN.flc files into the firmware's effect slots
(`effects_load_slots`), then compares `glyph_blit_effect`, `glyph_outline_blit`, `font_draw_char_effect` and the
texture builders with `sign_emu.Effects` frame by frame, and the Design tab's renderer (`core.render_message555`)
with the firmware drawing the same frames. Previews are panel frames at 40 fps (25 ms).
`test_font_import.py` imports every vendored pixel font, checks BDF glyphs bit for bit, has the firmware's
`font_build` measure each one (it must agree with the emulator).

## Money amounts
The Design tab's Value field is formatted the way the sign formats jackpot amounts (`amount_format`, firmware
0x479B4): value in minor units, decimals, separators, prefix/suffix, and four styles: plain; small cents (codes
0x00..0x09, superscript digits); odometer digits (codes 0x10..0x19, the fonts' bold digit set, and '$' -> 0x1C);
odometer digits with small cents. `core.amount_format` matches the firmware on 412 cases (`test_amount.py`).
Imported fonts get these glyph sets too: the family's bold digits where one is vendored (Tamzen, misc-fixed),
and small digits from the public-domain misc-fixed fonts.

## Fonts you can share
The sign's own fonts (fonte01..08) stay on your machine. Design tab -> "Import a free pixel font…" converts a font
from `third_party/fonts` (Spleen, Tamzen, X11 misc-fixed, Cozette, monogram, m5x7, Jersey 10; licences in
THIRD-PARTY.md) into a new `fonteNN.png` with a `fonteNN.font.json` sidecar holding its licence and credit line
(`font_import.py`, also usable from the command line). The Font field shows each font's origin and credit, and
whether it may be shared. Emoji need no special font: they are native emoji (docs/NATIVE-EMOJI.md).
