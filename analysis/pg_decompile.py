import sys, os
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("GHIDRA_INSTALL_DIR", os.path.join(REPO, "build", "ghidra_12.2_PUBLIC"))
import pyghidra
pyghidra.start()

from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

binpath, base, outbase = sys.argv[1], int(sys.argv[2], 16), sys.argv[3]
proj = os.path.join(REPO, "analysis", "ghidra_proj")

with pyghidra.open_program(binpath, project_location=proj, project_name="pg",
                           language="PowerPC:BE:32:default", analyze=True,
                           loader="ghidra.app.util.opinion.BinaryLoader") as api:
    prog = api.getCurrentProgram()
    # rebase to the requested base if not already
    mon = ConsoleTaskMonitor()
    di = DecompInterface(); di.openProgram(prog)
    fm = prog.getFunctionManager()
    total = fm.getFunctionCount()
    cw = open(outbase + ".c", "w"); lw = open(outbase + ".functions.txt", "w")
    n = ok = 0
    for f in fm.getFunctions(True):
        n += 1
        lw.write("%s  %s  (%d bytes)\n" % (f.getEntryPoint(), f.getName(), f.getBody().getNumAddresses()))
        cw.write("/* === %s  %s === */\n" % (f.getEntryPoint(), f.getName()))
        r = di.decompileFunction(f, 90, mon)
        if r and r.decompileCompleted():
            cw.write(r.getDecompiledFunction().getC()); ok += 1
        else:
            cw.write("// decompile failed: %s\n" % (r.getErrorMessage() if r else "null"))
        cw.write("\n")
        if n % 200 == 0:
            sys.stderr.write("  %d/%d\n" % (n, total)); sys.stderr.flush()
    cw.close(); lw.close()
    print("decompiled %d/%d functions -> %s.c" % (ok, n, outbase))
