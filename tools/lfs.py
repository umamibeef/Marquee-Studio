"""Moved to marquee_pack/lfs.py. This alias keeps `import lfs` (and running this file) working."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
if __name__ == "__main__":
    import runpy
    runpy.run_module("marquee_pack.lfs", run_name="__main__", alter_sys=True)
else:
    import importlib
    sys.modules[__name__] = importlib.import_module("marquee_pack.lfs")
