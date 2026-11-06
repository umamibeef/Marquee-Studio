# Sign emulator

The code now lives in `studio/marquee_studio/` (`sign_emu.py`, `font_render.py`). The files here are aliases.

This is a software model of the RGB555 LED panel, reconstructed from the firmware (see `../SPEC.md`). It defaults
to 80×16 and supports any of the panel sizes the firmware knows.

- `sign_emu.py` has `Panel` (RGB555 framebuffer), `FLIC` (.flc decoder) and GIF/PNG export.
  `python sign_emu.py <file.flc> [out.gif]` renders any extracted animation.
- `font_render.py` has `SignFont`, which loads a `fonte0N.png` atlas and draws proportional text in any colour.
- `demo.py` plays the boot logo, then a scrolling jackpot message, and writes `demo_sign.gif`.

Both modules were checked against real content: `sign_emu.py` renders the actual MIKOHN boot animation, and
`font_render.py` renders readable text from the extracted fonts. They need `pillow` and `numpy` (in `../venv`).

Example output: `demo_sign.gif` (full sequence), `logo_frames.png` (logo frames),
`fonte04_text.png` and `scroll_frame.png` (text).
