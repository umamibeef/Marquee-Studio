"""Moved to studio/marquee_studio/font_import.py. This alias keeps `import font_import` (and running this file) working."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
if __name__ == "__main__":
    import runpy
    runpy.run_module("marquee_studio.font_import", run_name="__main__", alter_sys=True)
else:
    import importlib
    sys.modules[__name__] = importlib.import_module("marquee_studio.font_import")
