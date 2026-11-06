import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.SourceType;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import java.io.*;
import java.util.*;

public class CoverCode extends GhidraScript {
    public void run() throws Exception {
        String out = getScriptArgs()[0];
        Listing lst = currentProgram.getListing();
        Memory mem = currentProgram.getMemory();
        // 1. Disassemble every undefined byte in an executable+initialized block, 4-byte aligned.
        int passes = 0, created = 0;
        boolean again = true;
        while (again && passes < 6 && !monitor.isCancelled()) {
            again = false; passes++;
            for (MemoryBlock b : mem.getBlocks()) {
                if (!b.isExecute() || !b.isInitialized()) continue;
                long CODE_END = 0x98800L;
                Address a = b.getStart();
                while (a != null && a.compareTo(b.getEnd()) < 0 && a.getOffset() < CODE_END && !monitor.isCancelled()) {
                    Data d = lst.getUndefinedDataAt(a);
                    if (d != null) {
                        // try to disassemble here if 4-aligned
                        if ((a.getOffset() & 3) == 0) {
                            DisassembleCommand dc = new DisassembleCommand(a, null, true);
                            if (dc.applyTo(currentProgram, monitor)) again = true;
                        }
                    }
                    Address next = a.next();
                    if (next == null) break;
                    a = next;
                }
            }
        }
        // 2. Create functions at every instruction that isn't already in one.
        InstructionIterator it = lst.getInstructions(true);
        FunctionManager fm = currentProgram.getFunctionManager();
        List<Address> starts = new ArrayList<>();
        Address prevEnd = null;
        for (Instruction ins : (Iterable<Instruction>)() -> it) {
            if (monitor.isCancelled()) break;
            Address ad = ins.getAddress();
            if (ad.getOffset() < 0x98800L && fm.getFunctionContaining(ad) == null) {
                if (prevEnd == null || ad.compareTo(prevEnd) > 0) starts.add(ad);
            }
            prevEnd = ins.getMaxAddress();
        }
        for (Address s : starts) {
            if (fm.getFunctionContaining(s) != null) continue;
            CreateFunctionCmd cf = new CreateFunctionCmd(s);
            if (cf.applyTo(currentProgram, monitor)) created++;
        }
        println("disasm passes=" + passes + " new funcs=" + created + " total=" + fm.getFunctionCount());
        // 3. Decompile all.
        DecompInterface di = new DecompInterface(); di.openProgram(currentProgram);
        PrintWriter cw = new PrintWriter(new FileWriter(out + ".c"));
        PrintWriter lw = new PrintWriter(new FileWriter(out + ".functions.txt"));
        int n = 0, ok = 0;
        for (Function f : fm.getFunctions(true)) {
            if (monitor.isCancelled()) break;
            n++;
            lw.println(f.getEntryPoint() + "  " + f.getName() + "  (" + f.getBody().getNumAddresses() + " bytes)");
            cw.println("/* === " + f.getEntryPoint() + "  " + f.getName() + " === */");
            DecompileResults r = di.decompileFunction(f, 90, monitor);
            if (r != null && r.decompileCompleted()) { cw.println(r.getDecompiledFunction().getC()); ok++; }
            else cw.println("// decompile failed: " + (r == null ? "null" : r.getErrorMessage()));
            cw.println();
            if (n % 300 == 0) cw.flush();
        }
        cw.close(); lw.close();
        println("DONE decompiled " + ok + "/" + n + " -> " + out + ".c");
    }
}
