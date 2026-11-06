# Pixel fonts and emoji for the sign

The sign is 80x16 pixels. The firmware draws text from `fonteNN.png` sheets
(a 16x16 or 16x8 grid of cells, proportional widths). The studio will convert
third-party pixel fonts into that format. We want to commit copies of these
fonts to the repo. The repo code is MIT.

So every font here must allow redistribution and modification. Allowed:
OFL 1.1, CC0, MIT, BSD, Apache-2.0, WTFPL, public domain, or a plain
"use, copy, modify, distribute" grant. Not allowed: non-commercial,
no-derivatives, "personal use only", unclear, or ripped from games.

Each licence below was checked on 2026-10-31 at the primary source (the
project's own LICENSE file, the author's page, or the itch.io "Asset
license" field set by the author). The survey downloaded only licence text.
Sizes and coverage come partly from the fonts' READMEs; for Creep, Proggy,
ProFont, Dina, Gohu, Kenney and Departure they were not checked against a
source. Check them again when you vendor a font.

Formats, best first: BDF (plain text, exact pixels), PCF, PNG sheet, then
TTF/OTF that is drawn on a pixel grid (render at the native pixel size with
anti-aliasing off).

## Verified fonts

Columns: H = pixel height of the cell (approx.). Prop = proportional,
Mono = monospace.

### 5 to 8 px (two lines of text on the sign)

| Font | Author | Licence (exact) | H | Coverage | Width | Convert from | Source / licence file | Obligations |
|---|---|---|---|---|---|---|---|---|
| Tiny5 | Gissio (GitHub) | SIL OFL 1.1, no Reserved Font Name | 5 (cap) | Latin ext., Greek, Cyrillic | Prop | TTF on grid | https://github.com/Gissio/font_tiny5 / https://github.com/google/fonts/blob/main/ofl/tiny5/OFL.txt | Keep OFL.txt. Do not sell alone. |
| Micro 5 | Soft Type (scfried) | SIL OFL 1.1, no RFN | 5 | Latin | Prop | TTF on grid | https://github.com/scfried/soft-type-micro / https://github.com/google/fonts/blob/main/ofl/micro5/OFL.txt | Keep OFL.txt. |
| Spleen 5x8 | Frederic Cambus | BSD 2-Clause | 8 | Latin-1 + Latin Ext-A + box drawing | Mono | BDF | https://github.com/fcambus/spleen / https://github.com/fcambus/spleen/blob/master/LICENSE | Keep copyright + licence text. |
| X11 misc-fixed (5x7, 5x8, 6x9) | Markus Kuhn et al. (ucs-fonts) | Public domain ("Public domain font. Share and enjoy.") | 7-9 | Wide (thousands of Unicode glyphs) | Mono | BDF | https://www.cl.cam.ac.uk/~mgk25/ucs-fonts.html (same page states the licence) | None. Keep a README with the source. |
| Silkscreen | Jason Kottke | SIL OFL 1.1, no RFN | 8 em, ~5 cap | Latin | Prop | TTF on grid | https://github.com/googlefonts/silkscreen / https://github.com/googlefonts/silkscreen/blob/main/OFL.txt | Keep OFL.txt. |
| Press Start 2P | Cody "CodeMan38" Boisclair | SIL OFL 1.1, Reserved Font Name "Press Start 2P" | 8 | Latin, Greek, Cyrillic | Mono | TTF on grid | https://github.com/google/fonts/tree/main/ofl/pressstart2p / https://github.com/google/fonts/blob/main/ofl/pressstart2p/OFL.txt | Keep OFL.txt. **Rename** any converted version. |
| monogram | datagoblin | CC0 1.0 (itch "Asset license"; author calls it public domain) | ~9 line, 5 wide | Latin ext., Greek, Cyrillic (extended version) | Mono | PNG sheet + JSON (monogram-bitmap.png) | https://datagoblin.itch.io/monogram (licence field on the page) | None. Credit is polite. |
| m5x7 | Daniel Linssen | CC0 1.0 (itch "Asset license"; "attribution appreciated") | 7 | ASCII + some Latin | Prop | TTF on grid | https://managore.itch.io/m5x7 (licence field on the page) | None. Credit is polite. |
| Kenney Fonts (Pixel, Mini, Mini Square, High, Blocks, ...) | Kenney | CC0 | ~6-12 | ASCII | Prop | TTF on grid | https://opengameart.org/content/kenney-fonts (uploaded by Kenney; licence CC0). kenney.nl returned 403 to the script. | None. |

### 8 to 12 px

| Font | Author | Licence (exact) | H | Coverage | Width | Convert from | Source / licence file | Obligations |
|---|---|---|---|---|---|---|---|---|
| Tamzen (5x9 ... 10x20) | Suraj N. Kurapati, based on Tamsyn by Scott Fial | Tamsyn/Tamzen free licence: "use, copy, modify, and distribute it as you see fit" | 9, 12, 13, 14, 15, 16, 20 | ASCII + Latin-1 + some symbols | Mono, regular + bold | BDF | https://github.com/sunaku/tamzen-font / https://github.com/sunaku/tamzen-font/blob/master/LICENSE | Keep LICENSE (both notices). |
| Cozette | Samhain (slavfox / the-moonwitch) & contributors | MIT | 13 (6x13 box) | Wide: Latin, Greek, Cyrillic, box, braille, Powerline, many icons | Mono | BDF | https://github.com/slavfox/Cozette / https://github.com/slavfox/Cozette/blob/master/LICENSE | Keep copyright + MIT text. |
| Creep | Romeo Van Snick | MIT | 11 | ASCII + some symbols | Mono (4 px wide) | BDF | https://github.com/romeovs/creep / https://github.com/romeovs/creep/blob/master/LICENSE | Keep MIT text. |
| Scientifica | Akshay Oppiliappan (nerdypepper) | SIL OFL 1.1, no RFN | 11 | ASCII + math/Greek/arrows | Mono, regular/bold/italic | BDF | https://github.com/nerdypepper/scientifica / https://github.com/nerdypepper/scientifica/blob/master/LICENSE | Keep LICENSE. |
| Gohufont | Hugo Chargois | WTFPL v2 | 11, 14 | ASCII + Latin-1 + box | Mono, regular + bold | BDF/PCF | https://font.gohu.org/ / https://github.com/hchargois/gohufont/blob/master/COPYING-LICENSE | None. |
| Proggy (Tiny, Clean, Square, ...) | Tristan Grimmer | MIT | 10-13 | ASCII + Latin-1 | Mono | PCF/BDF | https://github.com/bluescan/proggyfonts / https://github.com/bluescan/proggyfonts/blob/master/LICENSE | Keep MIT text. |
| Dina | Joergen Ibsen | Free: "use, distribute and modify it however you want, just don't ... claim that you made it" | 6/8/9/10 pt (~10-16 px) | ASCII + Latin-1 | Mono, regular + bold | FON (Windows); BDF ports exist | https://www.dcmembers.com/jibsen/download/61/ (licence on that page) | Credit the author. Keep the licence text. |
| ProFont | Andrew Welch, Carl Osterwald, Steve Gilardi; maintained by Tobias Jung | MIT (since 2014) | 9-12 (several sizes) | ASCII + Latin-1 | Mono | BDF/PCF (X11 pack) | https://tobiasjung.name/profont/ (licence stated there; LICENSE in the zip) | Keep MIT text. |
| Departure Mono | Helena Zhang & Tobias Fried | Font: SIL OFL 1.1, no RFN (the `LICENSE` in the release zip). The repository's MIT `LICENSE` covers its website code. | 11 | Latin ext., Greek, Cyrillic, box, symbols | Mono | OTF on 11 px grid | https://github.com/rektdeckard/departure-mono (release `v1.500`) | Keep the OFL text. |
| Jersey 10 | Soft Type (scfried) | SIL OFL 1.1, no RFN | 10 | Latin | Prop, bold display | TTF on grid | https://github.com/scfried/soft-type-jersey / https://github.com/google/fonts/blob/main/ofl/jersey10/OFL.txt | Keep OFL.txt. |
| SerenityOS bitmap fonts (Katica 10/12, Liza 10, Csilla 10/12, Tiny 6, Tinier 5, Pebbleton 14, ...) | SerenityOS developers | BSD 2-Clause (repo-wide LICENSE) | 5-14 | Latin + many extras | Prop and mono, several weights | SerenityOS `.font` binary (needs a small parser) | https://github.com/SerenityOS/serenity/tree/master/Base/res/fonts / https://github.com/SerenityOS/serenity/blob/master/LICENSE | Keep copyright + licence text. |
| Ark Pixel | TakWolf | SIL OFL 1.1, no RFN (fonts); MIT (build code) | 10, 12 (16 px was removed in 2026.09) | Wide: Latin + CJK | Mono and prop | BDF/PCF in releases | https://github.com/TakWolf/ark-pixel-font / https://github.com/TakWolf/ark-pixel-font/blob/master/LICENSE-OFL | Keep LICENSE-OFL. |
| Fusion Pixel | TakWolf | SIL OFL 1.1, no RFN | 8, 10, 12 | Wide: Latin + CJK (merged from several OFL fonts) | Mono and prop | BDF/PCF in releases | https://github.com/TakWolf/fusion-pixel-font / https://github.com/TakWolf/fusion-pixel-font/blob/master/LICENSE-OFL | Keep LICENSE-OFL and the upstream font notices shipped in the release. |

### 13 to 16 px (full height)

| Font | Author | Licence (exact) | H | Coverage | Width | Convert from | Source / licence file | Obligations |
|---|---|---|---|---|---|---|---|---|
| Terminus | Dimitar Toshkov Zhekov | SIL OFL 1.1, Reserved Font Name "Terminus Font" | 12, 14, 16 (also 18-32) | Wide: Latin, Greek, Cyrillic, box | Mono, normal + bold | BDF | https://terminus-font.sourceforge.net/ / OFL.TXT in the source tarball (checked via https://sources.debian.org/src/xfonts-terminus/4.49-1/OFL.TXT/) | Keep OFL.TXT. **Rename** any converted version. |
| Spleen 8x16 (also 6x12) | Frederic Cambus | BSD 2-Clause | 12, 16 | Latin-1 + Latin Ext-A + box | Mono | BDF | https://github.com/fcambus/spleen / https://github.com/fcambus/spleen/blob/master/LICENSE | Keep licence text. |
| X11 misc-fixed (6x13, 7x14, 8x13, 9x15, 9x18) | Markus Kuhn et al. | Public domain | 13-18 | Wide | Mono, some bold | BDF | https://www.cl.cam.ac.uk/~mgk25/ucs-fonts.html | None. |
| GNU Unifont | Roman Czyborra, Paul Hardy et al. | Fonts: SIL OFL 1.1 **and** GPL v2+ with the GNU font embedding exception (dual). Tools: GPL v2+ | 16 | Whole Unicode BMP; "Unifont Upper" covers higher planes | 8x16 / 16x16 cells | .hex (plain text) or BDF | https://unifoundry.com/unifont/ / https://unifoundry.com/LICENSE.txt | Vendor the fonts under the OFL option. Keep LICENSE.txt. Do not vendor the GPL tools. |
| unscii (8x8, 8x16) | Viznut | Public domain, **except** "unscii-16-full", which is GPL because it pulls in Unifont glyphs | 8, 16 | Wide: Latin, box, block graphics, PETSCII-style | Mono | .hex or BDF | http://viznut.fi/unscii/ (licence on that page) | None for the plain variants. Do not use unscii-16-full. |
| Pixel Operator (8 and 16 px families, Mono, Bold, HB) | Jayvee Enaguas (HarvettFox96) | CC0 1.0 | 8, 16 | Latin ext. | Prop and mono | TTF on grid | Original NotABug repo is gone (404). Licence checked in the fork: https://github.com/bauripalash/ttf-pixeloperator-fork / https://github.com/bauripalash/ttf-pixeloperator-fork/blob/master/LICENSE | None. Note the upstream is gone. |
| DotGothic16 | Fontworks | SIL OFL 1.1, no RFN | 16 | Wide: Latin + Japanese (JIS) | Mono-ish | TTF on 16 px grid | https://github.com/fontworks-fonts/DotGothic16 / https://github.com/google/fonts/blob/main/ofl/dotgothic16/OFL.txt | Keep OFL.txt. |
| VT323 | Peter Hull | SIL OFL 1.1, no RFN | ~14-16 | Latin ext. | Mono | TTF (render at 16 or 32 px and check the grid) | https://github.com/google/fonts/tree/main/ofl/vt323 / https://github.com/google/fonts/blob/main/ofl/vt323/OFL.txt | Keep OFL.txt. It redraws the DEC VT320 look; it is an original OFL release, not a ROM dump. |

## Checked when vendoring (2026-11-02)

Converting a TTF/OTF needs its outlines on a pixel grid (`font_import.read_ttf` checks every glyph). Results:
- Exact: Micro 5, Press Start 2P, Departure Mono, Jacquarda Bastarda 9, Jersey 10, Kenney Pixel, Pixel Square, Mini,
  Mini Square, Rocket, Blocks, High, m5x7, monogram. Vendored: see THIRD-PARTY.md.
- Not exact, not used:
  - VT323: drawn as CRT scan lines (rows of 40 units split 17/23, columns that overlap by 4 units);
  - DotGothic16: its points are on no grid (gcd 1);
  - Silkscreen (Google Fonts build): some glyphs have 5-unit slivers off its 125-unit grid, so `A`, `0`, `B` and
    others get partial pixels;
  - Jersey 15: FreeType's monochrome and anti-aliased renderings differ where contours overlap; its capitals are
    15 px tall anyway;
  - Kenney Future and Future Narrow: vector fonts, not pixel fonts;
  - Monocraft: its outlines use a grid twice as fine as its pixels (capitals 14 steps tall, strokes 2 steps), so
    it would convert at double size; the Bold is on no usable grid.
- Creep: the release BDF gives every glyph an advance of 7 px for 4-px glyphs. creep2 (github.com/raymond-w-ko/creep2,
  MIT) is the same design with a strict 5x11 box and is the one vendored.
- Tiny5 is now available as BDF (`fonts/bdf/Tiny5.bdf` in its repository); Google Fonts ships only a variable TTF.
- Pixel Operator: not vendored, because the author's repository (notabug.org) is gone and only forks remain.

## Recommended first picks

1. Spleen (BSD-2). BDF, so no rendering guesswork. 5x8 gives two lines,
   8x16 fills the panel, 6x12 sits between. One licence covers all sizes.
2. Tamzen (free, "as you see fit"). BDF in many heights (9-20) with bold.
   Good general text at 12-16 px. No rename rule.
3. X11 misc-fixed (public domain). The widest Unicode coverage in plain
   BDF, from 5x7 to 9x18. No obligations at all. A good fallback font.
4. Cozette (MIT). 13 px, clean, many symbols and icons. BDF.
5. monogram or m5x7 (CC0). Small, very readable game fonts for
   two-line text. m5x7 is proportional and monogram is mono. The monogram
   PNG sheet is close to our own format.
6. Jersey 10 or Press Start 2P (OFL) for bold display text. Jersey 10
   has no Reserved Font Name, so it is simpler. Press Start 2P must be
   renamed after conversion.

For wide Unicode at 16 px, add Unifont under its OFL option. It is 16 px
tall, so it leaves no space between lines.

## Emoji

Decision: the only emoji art is JOI3's Flat Pixel Art Emojis
(https://joi3.itch.io/flat-pixel-art-emojis), a pack the user bought. Its
licence does not allow redistributing it, so it stays in the git-ignored
`local/emoji-pack/` and only the native emoji set built from it is committed
(docs/NATIVE-EMOJI.md, THIRD-PARTY.md). The survey below is kept as
background; none of these sets is used.

### Purpose-made pixel-art emoji (best look at this size)

| Set | Author | Licence (exact) | Size | Count | Source / licence | Obligations |
|---|---|---|---|---|---|---|
| Pixel Art Emoji | Kuteken | CC0 1.0 (itch "Asset license") | most fit 16x16 | 149 | https://kuteken.itch.io/pixel-art-emoji | None. |
| Free Emoji Pack | actuallyKron | CC0 1.0 (itch "Asset license") | 16x16 | 64 | https://actuallykron.itch.io/free-emoji-pack | None. |
| Emotes pack | Kenney | CC0 (OpenGameArt, uploaded by Kenney) | pixel and vector styles; pixel size not checked | ~30 emotes x 16 styles | https://opengameart.org/content/emotes-pack | None. These are speech-bubble emotes, not Unicode emoji. |

The itch packs are small and not mapped to code points, so they would have
to be mapped by hand.

### Vector or colour emoji to downscale

These cover all of Unicode, but a 16 px downscale needs hand clean-up.

| Set | Licence (exact, from the repo) | Obligations | Source / licence |
|---|---|---|---|
| Twemoji (jdecked fork, continued after Twitter/X) | Graphics: CC BY 4.0. Code: MIT | Attribution. The README says a mention in the project README or an About section is enough. State that the images were changed. | https://github.com/jdecked/twemoji / https://github.com/jdecked/twemoji/blob/main/LICENSE-GRAPHICS |
| Noto Emoji (Google) | Font files: SIL OFL 1.1. Tools and most image resources (PNG/SVG in the repo): Apache 2.0. Flags: public domain | Using the images: keep a copy of Apache 2.0, keep any NOTICE, mark changed files. No attribution text in the UI is required. | https://github.com/googlefonts/noto-emoji / https://github.com/googlefonts/noto-emoji/blob/main/LICENSE |
| Blobmoji (Noto fork with the old blobs) | Apache 2.0 (images); OFL 1.1 (font) | Same as Noto. | https://github.com/C1710/blobmoji / https://github.com/C1710/blobmoji/blob/main/LICENSE |
| Fluent Emoji (Microsoft) | MIT | Keep the MIT text with "Copyright (c) Microsoft Corporation". | https://github.com/microsoft/fluentui-emoji / https://github.com/microsoft/fluentui-emoji/blob/main/LICENSE |
| OpenMoji | Graphics: CC BY-SA 4.0. Code: LGPL-3.0 | Attribution **and share-alike** (see below). | https://github.com/hfg-gmuend/openmoji / https://github.com/hfg-gmuend/openmoji/blob/master/LICENSE.txt |
| FxEmoji (Mozilla, unmaintained) | Visual design: CC BY 4.0. Code: Apache 2.0 | Attribution. | https://github.com/mozilla/fxemoji / https://github.com/mozilla/fxemoji/blob/gh-pages/LICENSE.md |

#### What share-alike means for OpenMoji

A 16 px cell made from an
OpenMoji SVG is an adaptation. Every converted cell, and any sheet that holds
them, must be released under CC BY-SA 4.0 (or a compatible licence) with
attribution. The MIT code in the repo is not affected; it is a separate
work. But the converted sheet cannot be MIT, and anyone who takes it must
keep BY-SA. That is legal, but it adds a third licence to track, so prefer
the sets listed above OpenMoji.

### Emoji recommendation

Superseded by the decision above: the bought pack is the only emoji art.

## Unverified or excluded

| Font / set | Why |
|---|---|
| m6x11 (Daniel Linssen) | The itch page says only "free to use with attribution". No licence field. It does not say we may redistribute or modify. Unclear, so excluded until the author confirms. (m5x7 by the same author is CC0.) |
| Tewi | The author removed it. The README says the licence is "somewhat unclear" because it is heavily derived from Monaco. Excluded. |
| ctrld-font | No licence file. It is forked from Dina and Proggy Tiny, which are permissive, but the fork itself has no grant. Excluded. |
| Dotto Emoji (16x16 pixel emoji) | Licence conflict: itch says CC BY 4.0, the GitHub repo LICENSE.md is CC BY-SA 4.0. Also still in development. Unverified. |
| 110 Free Pixelmojis (gabl18) | CC0 is verified, but the cells are 18x18, larger than the panel. Usable only after redraw. |
| envypn | Could not reach a primary licence file. Unverified. |
| Kenney.nl pages | kenney.nl blocked the script (403). The CC0 licence was checked on Kenney's own OpenGameArt uploads instead. |
| Pixelify Sans, Sixtyfour (OFL) | Licence is fine, but they are outline or variable fonts, not strict pixel grids. Low value for us. |
| Mutant Standard emoji | Known to be CC BY-NC-SA (non-commercial). Not checked here; excluded on that basis. |
| Apple Color Emoji, Segoe UI Emoji | Proprietary. Excluded. |
| Any font copied from a game or console ROM | Excluded by rule. |

## How to vendor

- Put each font under `third_party/fonts/<name>/`. Put emoji under
  `third_party/emoji/<name>/`.
- Copy the upstream licence file unchanged next to the font (`LICENSE`,
  `OFL.txt`, etc.).
- Add a short `SOURCE.md` (or a line in a top-level `third_party/README`):
  upstream URL, version or commit, date, and licence name.
- Converted `fonteNN.png` sheets are derived works. Keep them with, or point
  them to, the licence of the font they came from. The project MIT licence
  does not cover them.
- OFL Reserved Font Name: if the OFL notice names a Reserved Font Name
  (here: "Press Start 2P", "Terminus Font"), a modified version must not use
  that name. A converted sheet counts as modified. Name it, for example,
  "Sign Start 8" or "Marquee Term 16", and say in `SOURCE.md` that it is
  derived from the original. Fonts with no RFN (Tiny5, Silkscreen, Jersey 10,
  Scientifica, Ark, Fusion, DotGothic16, VT323, Unifont) can keep the name,
  but a clear "converted from X" note is still good practice.
- OFL also says the font may not be sold by itself. Bundling it in the repo
  is fine.
- Apache 2.0 sets (Noto, Blobmoji): keep the licence and any NOTICE file,
  and say in `SOURCE.md` that the images were changed.
- CC BY sets (Twemoji): put the credit line in the README and the studio
  About box.
- Do not vendor GPL-only files (Unifont tools, unscii-16-full).
