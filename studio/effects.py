"""Moved to studio/marquee_studio/diag_patterns.py. This alias keeps `import effects` (and running this file) working."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
if __name__ == "__main__":
    import runpy
    runpy.run_module("marquee_studio.diag_patterns", run_name="__main__", alter_sys=True)
else:
    import importlib
    sys.modules[__name__] = importlib.import_module("marquee_studio.diag_patterns")
