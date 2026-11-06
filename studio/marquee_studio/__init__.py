import os as _os, sys as _sys
_REPO = _os.path.dirname(_os.path.dirname(_os.path.dirname(_os.path.abspath(__file__))))
if _REPO not in _sys.path:
    _sys.path.insert(0, _REPO)
"""Umami Marquee Studio: content design for the ELITE_RGB LED sign, as a Python package.

Modules (each usable on its own):
  paths         where the repository, content and vendored files are
  sign_emu      the firmware-exact panel model: fonts (FwFont), text lines, effects, FLC decoding
  font_render   the older SignFont text renderer used by the emulator demos
  palette       named colours and the sign's colour codes
  config        protocols, the default sign configuration, display models
  fonts         the sign's fonts (fonteNN.png) as the firmware measures them
  effects       effect words (FG<<16|BG), effect slots, outline styles and colours
  render        the message renderer (render_message555) and frame packing
  markup        message-line markup: encode, decode, warnings, rendering
  amount        the sign's money formatter (amount_format) and Value fields
  flash         Flash On/Off times (milliseconds) and the photosensitivity check
  content       the content folder: assets, imports, the emoji catalogue and imported fonts
  font_import   vendored pixel fonts -> sign fonts
  native_emoji  native emoji: the emoji.bin sheet, UTF-8 encoding and the firmware's drawing rules
  diag_patterns approximations of the sign's DIAGNOSTICS test patterns
  signproto     sign protocol frames and CRCs (RS-485 records, download framing)
  lfs           the sign's LFS content archive format
  gif2flc       GIF -> FLC conversion as the firmware decodes FLC

The firmware-checked tests (studio/test_*.py) cover these modules; see studio/README.md.
"""
