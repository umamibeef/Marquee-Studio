"""Moved to marquee_pack/font_import.py. This alias keeps `from marquee_studio import font_import` working for the
Studio and the dev/sim tools; the firmware build imports marquee_pack directly.
"""
import importlib, sys
sys.modules[__name__] = importlib.import_module("marquee_pack.font_import")
