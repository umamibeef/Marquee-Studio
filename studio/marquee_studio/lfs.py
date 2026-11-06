"""Moved to marquee_pack/lfs.py. This alias keeps `from marquee_studio import lfs` working for the
Studio and the dev/sim tools; the firmware build imports marquee_pack directly.
"""
import importlib, sys
sys.modules[__name__] = importlib.import_module("marquee_pack.lfs")
