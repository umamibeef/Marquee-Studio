# Ghidra Jython: decompile every function to one .c file + a listing.
import os
from ghidra.app.decompiler import DecompInterface
args = getScriptArgs()
out = args[0] if args else "/tmp/decomp"
di = DecompInterface(); di.openProgram(currentProgram)
cw = open(out + ".c", "w"); lw = open(out + ".functions.txt", "w")
fm = currentProgram.getFunctionManager()
n = ok = 0
for f in fm.getFunctions(True):
    if monitor.isCancelled(): break
    n += 1
    lw.write("%s  %s  (%d bytes)\n" % (f.getEntryPoint(), f.getName(), f.getBody().getNumAddresses()))
    cw.write("/* %s  %s */\n" % (f.getEntryPoint(), f.getName()))
    r = di.decompileFunction(f, 60, monitor)
    if r is not None and r.decompileCompleted():
        cw.write(r.getDecompiledFunction().getC()); ok += 1
    else:
        cw.write("// decompile failed\n")
    cw.write("\n")
cw.close(); lw.close()
print("decompiled %d/%d functions -> %s.c" % (ok, n, out))
