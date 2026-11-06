# Third-party code in this repository

The code written for this project is under the MIT licence (`LICENSE`). The folders listed here are copies of
other projects' source code and stay under their own licences, with their licence files and copyright headers
kept as they came. The MIT licence does not apply to them.

All copies are under `third_party/`. Each code copy is the upstream source at the named tag, with these changes only:
- the `.git` folders were left out;
- binary files were removed (see "Removed files" below), because this repository holds text only (the fonts
  below are the one exception);
- for RTEMS and newlib, only the parts this project reads were copied (see each entry).

No file in `third_party/` has been edited. If one ever is, record the change here and in the file itself, as the
GPL requires for RTEMS.

| Folder | Project | Version (upstream tag, commit) | Licence | Licence file | Used by |
|---|---|---|---|---|---|
| `third_party/lwip` | lwIP TCP/IP stack | `STABLE-2_2_1_RELEASE`, `77dcd25a`, github.com/lwip-tcpip/lwip | BSD 3-clause | `COPYING` | the boot loader's network window (`bootloader/`, `NET=1`), compiled in |
| `third_party/rtems-4.6.0` | RTEMS real-time OS | `4.6.0`, `f8105775`, github.com/RTEMS/rtems (cpukit, the MPC8xx BSP, optman) | GPL v2 or later **with the RTEMS linking exception**. The networking code is BSD; RPC/XDR and the web server have their own licences. | `LICENSE`, `COPYING`, `LICENSE.NET`, `LICENSE.RPCXDR`, `LICENSE.WEBSERVER` | `analysis/tools/libmatch.py`, to identify RTEMS functions in the firmware. Not compiled into anything shipped. |
| `third_party/newlib-1.11.0` | newlib C library | `newlib-1_11_0`, `34e3db62`, sourceware.org/git/newlib-cygwin.git | several free licences, file by file, mostly BSD-style | `COPYING.NEWLIB` (lists them), `COPYING`, `COPYING.LIB` | `libmatch.py`, to identify C library functions |
| `third_party/zlib-1.1.4` | zlib | `v1.1.4`, `a383133c`, github.com/madler/zlib | zlib licence | the notice in `README` and `zlib.h` | `libmatch.py` |
| `third_party/libpng-1.2.5` | libpng | `v1.2.5`, `0cc2f95a`, github.com/pnggroup/libpng | libpng licence | `LICENSE` | `libmatch.py` |
| `third_party/imgui` | Dear ImGui (docking branch) | `v1.92.9b-docking`, github.com/ocornut/imgui (core and the SDL3 / SDL_Renderer backends) | MIT | `LICENSE.txt` | `umami-marquee-sim`, the simulator's window (`simulator/`), compiled in |
| `third_party/stb` | stb_image, stb_image_write, stb_truetype, Sean Barrett | commit `f58f558c`, github.com/nothings/stb (`SOURCE.txt`) | MIT or public domain (Unlicense), at the user's choice | `LICENSE` | the C++ studio (`simulator/src/studio/`): reading and writing PNG/GIF, TrueType font import |
| `third_party/nlohmann` | JSON for Modern C++, Niels Lohmann | `v3.11.3`, the single header `json.hpp` (`SOURCE.txt`) | MIT | `LICENSE.MIT` | the C++ studio: the config file and the font sidecars |
| `third_party/fonts-ui` | Fira Sans (Regular, SemiBold), Fira Code (Regular), Font Awesome 6 Free (Solid), IconFontCppHeaders | Fira Sans from github.com/google/fonts `ofl/firasans`; Fira Code `6.2` (github.com/tonsky/FiraCode release); Font Awesome `6.7.2` (github.com/FortAwesome/Font-Awesome `webfonts/fa-solid-900.ttf`); `IconsFontAwesome6.h` from github.com/juliettef/IconFontCppHeaders | SIL OFL 1.1 (the fonts); Font Awesome Free: the font files SIL OFL 1.1, the icons CC BY 4.0; IconFontCppHeaders zlib licence | `OFL-FiraSans.txt`, `OFL-FiraCode.txt`, `LICENSE-FontAwesome.txt`, `LICENSE-IconFontCppHeaders.txt` | `umami-marquee-sim`'s window: the UI, code and icon fonts (`simulator/src/ui_style.cpp`), loaded at run time |

## Fonts (`third_party/fonts/`) and emoji

Pixel fonts for the studio to convert into sign fonts. Each folder keeps its licence file. Fetched on
2026-10-31 from the primary sources below; the survey and the licence checks are in `docs/PIXEL-FONTS.md`.
Fonts added on 2026-11-02 were fetched that day from the sources in their rows. These are the only binary files in
the repository (`.png`, `.ttf`, `.otf`; `.bdf` and `.hex` are text); `tools/check_no_binaries.sh` allows binaries
under `third_party/fonts/` and nowhere else.

| Folder | Font | Source (version) | Licence | What is copied |
|---|---|---|---|---|
| `spleen` | Spleen, Frederic Cambus | github.com/fcambus/spleen (commit `5116f8e`) | BSD 2-Clause (`LICENSE`) | BDF 5x8, 6x12, 8x16; `README.md`, `AUTHORS` |
| `tamzen` | Tamzen, Suraj N. Kurapati, from Tamsyn by Scott Fial | github.com/sunaku/tamzen-font (commit `3255e82`) | Tamsyn/Tamzen licence: "use, copy, modify, and distribute it as you see fit" (`LICENSE`) | all `Tamzen*.bdf` (5x9 to 10x20, regular and bold) |
| `misc-fixed` | X11 misc-fixed, Markus Kuhn and contributors | www.cl.cam.ac.uk/~mgk25/download/ucs-fonts.tar.gz | public domain: each BDF says `COPYRIGHT "Public domain font. Share and enjoy."` | the 22 BDF files whose header says public domain, and the package README (as `README.ucs-fonts`). `helvR12` (Adobe/DEC) and `clR6x12` (Schumacher) are left out because they are not public domain. |
| `cozette` | Cozette, Samhain and contributors | github.com/the-moonwitch/Cozette, release `v.1.30.0` | MIT (`LICENSE`) | `cozette.bdf` |
| `jersey10` | Jersey 10, The Soft Type Project Authors | github.com/google/fonts `ofl/jersey10` | SIL OFL 1.1, no Reserved Font Name (`OFL.txt`) | `Jersey10-Regular.ttf` |
| `monogram` | monogram, Vinícius Menézio | datagoblin.itch.io/monogram (files of 2021-2024) | CC0 1.0 (`credits.txt` and the itch.io asset licence; `LICENSE.md` records this) | `monogram-bitmap.png` + `.json` (a ready glyph sheet), `monogram-italic-bitmap.png`, `monogram.ttf`, `monogram-extended.ttf`, `credits.txt` |
| `m5x7` | m5x7, Daniel Linssen | managore.itch.io/m5x7 | CC0 1.0 (the itch.io asset licence; the download has no licence file, `LICENSE.md` records the page) | `m5x7.ttf` |
| `tiny5` | Tiny5, Gissio (The Tiny5 Project Authors) | github.com/Gissio/font_tiny5 (commit `55d314b`, `fonts/bdf/`) | SIL OFL 1.1, no Reserved Font Name (`OFL.txt`) | `Tiny5.bdf` |
| `micro5` | Micro 5, The Soft Type Project Authors | github.com/google/fonts `ofl/micro5` (commit `2fe1e8c`; upstream github.com/scfried/soft-type-micro) | SIL OFL 1.1, no Reserved Font Name (`OFL.txt`) | `Micro5-Regular.ttf` |
| `jacquardabastarda9` | Jacquarda Bastarda 9, The Soft Type Project Authors | github.com/google/fonts `ofl/jacquardabastarda9` (commit `2fe1e8c`; upstream github.com/scfried/soft-type-jacquarda-bastarda) | SIL OFL 1.1, no Reserved Font Name (`OFL.txt`) | `JacquardaBastarda9-Regular.ttf` |
| `pressstart2p` | Press Start 2P, Cody "CodeMan38" Boisclair (The Press Start 2P Project Authors) | github.com/google/fonts `ofl/pressstart2p` (commit `2fe1e8c`) | SIL OFL 1.1 **with Reserved Font Name "Press Start 2P"** (`OFL.txt`) | `PressStart2P-Regular.ttf` (unchanged; a converted copy is named "Sign Start 8") |
| `departure-mono` | Departure Mono, Helena Zhang | github.com/rektdeckard/departure-mono, release `v1.500` (`DepartureMono-1.500.zip`) | SIL OFL 1.1, no Reserved Font Name (`LICENSE` from the release zip; the repository's own `LICENSE` is MIT and covers its website code) | `DepartureMono-Regular.otf`, `LICENSE` |
| `scientifica` | scientifica, Akshay Oppiliappan | github.com/oppiliappan/scientifica, release `v2.3` (`scientifica.tar`) | SIL OFL 1.1, no Reserved Font Name (`LICENSE` from the repository) | `scientificaItalic-11.bdf` |
| `creep2` | creep2, Raymond W. Ko, a redrawing of creep by Romeo Van Snick with a strict 5x11 box | github.com/raymond-w-ko/creep2 (commit `69dc0de`) | MIT (`LICENSE`) | `creep2-11.bdf` |
| `kenney` | Kenney Fonts, Kenney | kenney.nl/assets/kenney-fonts (the download zip) | CC0 1.0 (`License.txt` from the zip) | `Kenney Rocket.ttf`, `Kenney Blocks.ttf`, `Kenney High.ttf` |
| `unscii` | unscii, Viznut | viznut.fi/unscii (the `.hex` files on that page; newest source release 2.1) | public domain, all variants except `unscii-16-full` (GPL), which is not copied (the download has no licence file; `LICENSE.md` records the page) | `unscii-16.hex`, `unscii-8-mcr.hex`, `unscii-8-fantasy.hex`, `unscii-8-tall.hex` |

When the studio converts these into `fonteNN.png`:
- keep each font's licence with the converted file, and its copyright notice in the studio's credits;
- OFL without a Reserved Font Name (Jersey 10, Tiny5, Micro 5, Jacquarda Bastarda 9, Departure Mono, scientifica): a
  converted font may keep its name, and stays under the OFL;
- OFL with a Reserved Font Name (Press Start 2P): a converted font is a Modified Version and must not use that name.
  `font_import.py` names it "Sign Start 8" (`RENAMED`; the sidecar's `name` and `note`), credits Press Start 2P as
  its origin, and it stays under the OFL;

Emoji: the only emoji art is JOI3's Flat Pixel Art Emojis (https://joi3.itch.io/flat-pixel-art-emojis), a pack
the user bought. It is not in this repository and is not redistributed. Its licence allows modifying and using the
art in a project (commercial too) but not redistributing the asset, so the pack stays in the git-ignored `local/`
folder, and only the built native emoji set (`firmware/emoji/16/emoji.bin`) is committed (docs/NATIVE-EMOJI.md).
Credit is not required; it is given here.

## Front end: shadcn/ui components (`frontend/packages/ui/`)

The studio's web UI uses shadcn/ui components, vendored: copied once into `frontend/packages/ui/src/` with
the shadcn CLI (`shadcn@4.23.0`, style `new-york`, base `radix`, 2026-10-31) and owned by this project from then on
(docs/PORTAL-PLAN.md §7b). Nothing is fetched from the shadcn registry at build or run time; updating a component
is a deliberate CLI run, reviewed as a diff.

| What | Project | Licence | Where |
|---|---|---|---|
| UI components: `accordion`, `alert`, `alert-dialog`, `badge`, `button`, `card`, `checkbox`, `collapsible`, `command`, `dialog`, `dropdown-menu`, `empty`, `hover-card`, `input`, `kbd`, `label`, `popover`, `progress`, `radio-group`, `scroll-area`, `select`, `separator`, `sheet`, `sidebar`, `skeleton`, `slider`, `sonner`, `switch`, `table`, `tabs`, `textarea`, `toggle`, `toggle-group`, `tooltip` (in `src/components/ui/`), the `use-mobile` hook | shadcn/ui, shadcn (ui.shadcn.com, github.com/shadcn-ui/ui) | MIT | `frontend/packages/ui/src/components/ui/`, `src/hooks/use-mobile.ts` |
| Theme tokens (the CSS variables in `src/styles/globals.css`, zinc base) | shadcn/ui | MIT | `frontend/packages/ui/src/styles/globals.css` (with our `--stage` tokens added) |

Changes after the CLI copy: none in the components. `globals.css` was written by hand from shadcn's documented
tokens, because the CLI's `init` needs a framework project and `packages/ui` is a plain package.

The npm packages these use (Radix UI via `radix-ui`, `cmdk`, `sonner`, `lucide-react`, `next-themes`, `cn`,
`class-variance-authority`, `tw-animate-css`) and the build tools (React, Vite, Tailwind CSS, TanStack Query,
React Router, TypeScript, ESLint, Playwright) are installed from npm at the versions pinned in
`frontend/package-lock.json`; they are not copied into the repository (`node_modules/` is ignored). All are MIT,
except TypeScript, Playwright and class-variance-authority (Apache 2.0) and lucide-react (ISC).

## What each licence asks of us

This is a summary to keep the repository in order. It is not legal advice; each licence file is the authority.
- lwIP, zlib, libpng and newlib (BSD-style, zlib and libpng licences): keep the copyright notices and licence
  files. lwIP's notice must also go with any binary we distribute, for example a boot loader image built with
  `NET=1`.
- RTEMS (GPL v2+ with exception):
  - keep `LICENSE` and `COPYING` and every file header;
  - give the source with any copy (it is source here);
  - mark any file we change, and keep changed files under the same licence.

  The exception means a program *linked with* RTEMS does not become GPL because of that alone. Nothing in this
  project links with it: `libmatch.py` only parses the files.
- Our MIT code sits next to these copies but is not combined with RTEMS. The boot loader rebuild links only
  our code and lwIP (BSD), so it stays MIT.

## What was copied from RTEMS and newlib

- RTEMS 4.6.0 (about 14 MB of the 57 MB tree):
  - the top-level licence files and `README`;
  - all of `cpukit/`;
  - `c/src/lib/libcpu/powerpc/`;
  - `c/src/lib/libbsp/powerpc/mbx8xx/` and `c/src/lib/libbsp/powerpc/shared/` (the MPC8xx board support the
    firmware was built from).
  - `c/src/optman/` (the optional-manager stubs, matched to 30 "not configured" functions in the firmware).
- newlib 1.11.0: the top-level `COPYING*` and `README`, and `newlib/libc/` whole.

`libmatch.py` reads exactly these folders. For the rest of the trees, use the upstream tags above.

## Removed files (binary)

These were left out of the copies. All of them are test images, icons, project files or fuzzing inputs that this
project does not use:
- libpng 1.2.5:
  - `pngbar.jpg`, `pngbar.png`, `pngnow.png`, `pngtest.png`;
  - the 16 PngSuite images in `contrib/pngsuite/`;
  - `contrib/gregbook/toucan.png`;
  - `contrib/visupng/VisualPng.ico` and `contrib/visupng/VisualPng.png`;
  - `projects/beos/x86-shared.proj` and `projects/beos/x86-static.proj`.
- lwIP:
  - `contrib/apps/LwipMibCompiler/SharpSnmpLib/sharpsnmplib.snk`;
  - `contrib/examples/httpd/examples_fs/img/sics.gif` and `src/apps/http/fs/img/sics.gif`;
  - the fuzzing inputs `test/fuzz/inputs/*/*.bin` (arp, icmp, ipv6 × 2, tcp, udp).

## Not included in this repository

- The sign's firmware, its fonts, animations, sounds and logos, and decompiler output of them belong to the
  sign's maker. A licence file cannot make them shareable, so they are not in this repository (see README).
- JOI3's Flat Pixel Art Emojis, the bought emoji pack: local only (`local/emoji-pack/`, git-ignored), never
  committed. Only the native emoji set built from it is in the repository.
- RTEMS 4.6.6, used once to compare versions, is not copied; `libmatch.py` uses 4.6.0.

## Our own helper files next to these

`analysis/libstub/` holds small stub headers written for this project (MIT). They let clang parse the sources
above. Its `rtemsstub/` links point into `third_party/rtems-4.6.0`.
