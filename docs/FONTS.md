# Fonts on the sign

## The sign's own fonts and ours

The stock firmware loads fonts only as `fonte00.png` .. `fonte99.png` (`fonts_load`, 0x44EA4): a PNG with a 16 x 16
grid of all 256 codes, the 5th letter of the name choosing the layout (`e`: 16 x 16 grid), each glyph's width guessed
by trimming its ink, digits forced to the full cell. The sign's own fonts are `fonte01.png` .. `fonte08.png`.

Our firmware (`firmware/ext/fonts.c`) keeps those, then loads our fonts **by name** from the list `fonts.lst`, in our
own format, MQF. Added fonts have real names (`spleen-8x16.mqf`), not fonteNN numbers. The menu font that the stock
firmware compiled in as font 0 is gone with the menu (`firmware/ext/nomenu.c`).

Font numbers (`ESC \` n` in message markup) follow the order the firmware loads them: the sign's fonte files, then our
fonts sorted by name. The web Studio lists them in the same order (`studio/marquee_studio/fonts.py`
`sign_font_names`).

## MQF1

`studio/marquee_studio/mqf.py` writes and reads it; `firmware/ext/fonts.c` and
`frontend/packages/sign-emu/src/mqf.ts` load it. Big-endian:

```
header  16 bytes: "MQF1", line height u8, ascent u8, flags u8 (1: body zlib-compressed), 0,
        ranges u16, glyphs u16, unpacked body length u32
body    ranges   8 bytes each: first code u32, count u16, first glyph u16
        advances 1 byte per glyph
        cells    per glyph: line height rows of ceil(advance / 8) bytes, MSB first: the glyph's advance box
```

Each glyph is stored as its designer's advance box, so its bearings and spacing are the font's own; one byte of
metrics per glyph; any code ranges. On the converted fonts it is about 20% smaller than the same fonts as PNG grids.

The firmware lays each glyph into a cell grid image and gives every code the width of its advance, not proportional,
so the stock text engine draws each glyph exactly its advance wide, with no inserted gap columns.

## Converting

`studio/marquee_studio/font_import.py` converts vendored pixel fonts (`third_party/fonts`, licences in
`THIRD-PARTY.md`): BDF and Unifont `.hex` exactly, TTF/OTF pixel fonts at their design size with every glyph checked
against an independent rasteriser. `firmware/fonts.txt` is the list our firmware image carries; the image build
(`bootloader/Makefile`) converts it and writes `fonts.lst`, and `font_import.py --set firmware/fonts.txt
studio/content/fonts` puts the same fonts in the Studio.

## Checks

- every converted glyph against its source (`studio/test_font_import.py`);
- text in MQF fonts drawn by our firmware in the simulator against the Studio's emulation, frame for frame
  (`frontend/packages/sign-emu/test/appvm_firmware.test.ts`).
