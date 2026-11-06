#!/usr/bin/env python3
"""Compatibility front for the studio's older code (server.py, the tests): everything that used to live here is
now in the marquee_studio package, one module per concern (see marquee_studio/__init__.py). New code should
import the modules directly, e.g. `from marquee_studio.markup import encode_markup`."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from marquee_studio import paths  # noqa: E402
from marquee_studio import sign_emu as emu  # noqa: E402,F401
from marquee_studio import font_import, lfs  # noqa: E402,F401
from marquee_studio.sign_emu import Panel, FLIC, rgb888_to_rgb555, rgb555_to_rgb888  # noqa: E402,F401
from marquee_studio.font_render import SignFont  # noqa: E402,F401
from marquee_studio.palette import *  # noqa: E402,F401,F403
from marquee_studio.palette import _colour  # noqa: E402,F401
from marquee_studio.config import *  # noqa: E402,F401,F403
from marquee_studio.fonts import *  # noqa: E402,F401,F403
from marquee_studio.fonts import _font, _fwfont, _FONTS  # noqa: E402,F401
from marquee_studio.effects import *  # noqa: E402,F401,F403
from marquee_studio.effects import _outline, _SLOTS  # noqa: E402,F401
from marquee_studio.amount import *  # noqa: E402,F401,F403
from marquee_studio.flash import *  # noqa: E402,F401,F403
from marquee_studio.render import *  # noqa: E402,F401,F403
from marquee_studio.render import _glyph_xs, _blit_text  # noqa: E402,F401
from marquee_studio.markup import *  # noqa: E402,F401,F403
from marquee_studio.markup import _text_bytes, ESC_, CR_  # noqa: E402,F401
from marquee_studio.content import *  # noqa: E402,F401,F403
from marquee_studio.content import _flc_author_name, _next_font_name  # noqa: E402,F401
from marquee_studio import diag_patterns as effects  # noqa: E402,F401  (the old name of that module)

HERE = os.path.dirname(os.path.abspath(__file__))
CONTENT = paths.CONTENT
