# Programmable apps

An app is a small program that draws its own line on the sign. It is written in a restricted C against
`apps/include/sign.h`, compiled by clang to eBPF bytecode, and run by an interpreter in our firmware. The web Studio
runs the same bytecode in a port of the same interpreter for its preview, so the preview is what the sign draws: the
test `frontend/packages/sign-emu/test/appvm_firmware.test.ts` checks it frame for frame against the firmware in the
simulator. The firmware knows nothing about particular apps; the clock, counter, countdown, rotating text and
weather are all apps written this way (`apps/`).

## The pieces

| Part | Where | What it does |
|---|---|---|
| App source | `apps/<name>/app.c`, `app.json`, optional `assets/` | C against `sign.h` (and `show.h`); the manifest lists the app's settings and feed; pictures and animations it bundles |
| Toolchain | Homebrew LLVM: `clang -target bpfel -mcpu=v4 -O2` | eBPF v4 (RFC 9669), little-endian. Apple's clang has no BPF target |
| Bundler | `tools/mkapp.py` | ELF object -> MQAP1 bundle: code, data, the instance's settings, assets, manifest; every relocation resolved at build time |
| Interpreter (sign) | `firmware/ext/ebpf.c` | freestanding C, interpreter only |
| Interpreter (Studio) | `frontend/packages/sign-emu/src/ebpf.ts` | a line-by-line port of `ebpf.c` |
| Runtime (sign) | `firmware/ext/apps.c` | loads bundles, runs them, the helpers, the attract-loop take-overs |
| Runtime (Studio) | `frontend/packages/sign-emu/src/appvm.ts`, `attract.ts` | the same helpers over the Studio's emulation of the sign's drawing |
| Content store | `firmware/ext/store.c` | files uploaded over the DB9, kept in the main flash |
| Upload | `studio/marquee_studio/store_upload.py`, `sign_window.py`, server `/api/upload_apps` | builds the instances; umami-marquee-sim's Sign window (Content tab) sends them through the DB9 console |
| Feeds | `studio/marquee_studio/feeds/` | data for data apps (weather), sent by the Studio's server |

Both interpreters pass all 313 programs of the eBPF conformance suite (`third_party/bpf-conformance`, assembled by
`tools/ebpf_asm.py`): the C one natively (`firmware/ext/test/ebpf_conformance.c`, in `check.sh`), the TypeScript one
under Node (`test/ebpf.test.ts`). Memory is little-endian on both sides whatever the host, so the suite applies as is.

## The app's view of the sign

An app owns a **buffer** as big as the panel (`width()`, `height()`). The firmware calls two functions:

- `int init(void)`: once, when the app is loaded. Non-zero means it failed.
- `int frame(unsigned t)`: once per 40 fps frame while its line is on screen, `t` counting from the start of its
  turn. It returns 0 to go on, 1 when its turn is over; the attract loop then moves to the next line.

Its globals keep their values between frames and turns. Everything else goes through **helpers** (`call N`); the
numbers are the ABI:

| # | Helper | Does |
|---|---|---|
| 1, 2 | `width()`, `height()` | the panel's size |
| 3 | `clear(paint)` | the whole buffer |
| 4 | `rect(x, y, w, h, paint)` | a filled rectangle |
| 5 | `pixel(x, y, rgb)` | one pixel |
| 6 | `text(font, x, y, paint, s)` -> width | a string in a sign font, drawn by the firmware's own text engine (native emoji included) |
| 7, 8 | `text_width(font, s)`, `font_height(font)` | measuring |
| 9 | `font_find(name)` | a font by name ("spleen-8x16.mqf", "fonte04.png"), -1 if the sign lacks it |
| 10, 11, 12 | `image(x, y, asset, frame)`, `asset_find(name)`, `asset_info(asset, what)` | the app's bundled pictures and animations |
| 13, 14 | `now()`, `fmt_time(buf, len, fmt, t)` | the sign's wall-clock time (-1 until set), strftime |
| 15, 16 | `setting_int(key, dflt)`, `setting_str(key, buf, len)` | the app's settings |
| 17, 18, 19 | `data_int(key, dflt)`, `data_str(key, buf, len)`, `data_age(key)` | values pushed to the app, and their age in seconds |
| 20 | `outline(style, colour)` | the sign's outline around the letters drawn this frame |
| 21 | `print(s)` | a line on the DB9 console |

A **paint** is an RGB555 colour, or one of the firmware's effect fills: `PAINT_WIPE`, `PAINT_STRIPES`,
`PAINT_SLOT(n)` (the sign's effect animation n), drawn with the firmware's own effect blit at the current frame's
phase. `show.h` has the usual way to show a line of text (held or scrolling, colours, outline, font from the
settings), plays a bundled `background` animation behind it, and has `expand()` for templates such as
`{temp}° {sky}`. In the Studio, "Animation behind the text" bundles any animation of the library (imported GIFs
included) with the app as that asset.

## Safety

- At load the bytecode is checked: every opcode, register, jump target and helper number.
- Every load and store is checked against the app's regions (its stack, 512 bytes per call frame, and its data).
  Strings passed to helpers must lie in them and end within 255 bytes.
- Each `init()` and `frame()` may execute at most 200,000 instructions. The apps here use 70 to 420 per frame.
- A stopped app shows a line saying why (`app 3: out of time at pc 812`) for 3 seconds instead of its turn; the
  console's `apps` command lists errors. The rest of the sign carries on.

## Bundles, instances and the store

`tools/mkapp.py apps/clock out.mqap format=%l:%M` builds an **instance**: the app's code and data with one set of
settings. MQAP1 is big-endian, with little-endian code:

```
header    40 bytes: "MQAP", ABI u16 (1), flags u16, code, data, bss bytes, init and frame instruction, settings,
          assets, manifest bytes (u32 each)
code      eBPF; 64-bit address loads already point into the data segment (fixed at 0x20000000)
data      initialised data; the bss follows it in the app's memory, zeroed
settings  key=value NUL ...: Latin-1 text, native emoji as UTF-8
assets    pictures and animations (PNG, GIF, FLC) as RGB555 frames (bit 15 = transparent), zlib-compressed;
          unpacked once when the app is loaded
manifest  app.json, for the Studio
```

The app on attract line N is the file `appN.mqap` in the **content store**: two clocks with different formats are
two files. The store is our part of the main flash's archive chain (volume 1, at 0x50000000). It starts on a 64 KB
sector boundary after any files already there, behind a marker entry `mq.store`, so rewriting it erases only its own
sectors. The firmware itself reads only the copy of the archive it makes in RAM at boot, so the store is read through
`store_find`, never `lfs_open`. Writing uses the boot loader's AMD erase and program polling, with interrupts off
during each operation and the watchdog serviced. A commit makes the store's lines active (on, with a transparent
effect) and saves that in the firmware's own settings.

## How apps share the panel

When the attract loop reaches an app's line, our take-overs run the app in place of the text line:
`textline_init` loads it (and runs `init()` once), `textline_step` runs `frame(t)` and ends the line when it returns
1, and `frame_submit` puts the app's buffer on the panel. One black frame follows each line, as for text lines.

## Console commands (DB9)

| Command | Does |
|---|---|
| `sbegin`, `sfile NAME SIZE`, `sraw SIZE` + bytes, `scommit` | replace the content store with the files sent (raw pieces of 256 bytes, each acknowledged, so the sign's receive buffer cannot overrun; `sd HEX` also works) |
| `sls` | the files in the store |
| `time SECS` | the sign's clock (hex local wall-clock seconds) |
| `adata LINE KEY HEX` | a value for the app on LINE |
| `apps` | the apps loaded, their errors, and the runtime's counters |

## Data feeds

The sign has no network connection, so data apps get their data from the Studio's server over the DB9. A feed
(`studio/marquee_studio/feeds/<name>.py`) returns `{key: text}` for an app's settings; after an upload the server
sends the values (`adata`) and keeps doing so on the feed's schedule while it runs. Weather uses Open-Meteo, which
needs no account or key: temp, feels, hi, lo, wind, sky (an emoji from the sign's emoji set, or a word) and sky_text.
Without fresh data the weather app shows `--` after its `max_age`.

## The Studio

- The Apps page lists the apps of `apps/` (`/api/apps`), puts them on lines with their settings (a form made from
  each `app.json`, plus the look `show.h` reads: letters, background, outline, font, display), and saves them.
- Its preview builds each instance (`/api/app_bundle`: bytes, built by the server) and runs it in the browser, with
  the feeds' current values (`/api/feed/<name>`).
- Compose's "Show on the sign" makes the design a Text app instance on a line.
- Upload (`/api/upload_apps`) needs umami-marquee-sim running: the Studio hands the upload to its Sign window
  (Content tab), which shows every step and does the serial work. It builds the instances, writes the store, sets the sign's time and sends the feeds'
  values, to the sign or to the simulator (`build/sim/db9`).

## Writing an app

```c
#include "show.h"

int init(void) { show_init(); return 0; }

int frame(unsigned t)
{
	char s[32];
	str_int(s, data_int("visitors", 0));
	return show_frame(s, t);
}
```

with an `app.json` naming its settings. Put it in `apps/<name>/`; the Studio lists it. `check.sh` builds every app.
