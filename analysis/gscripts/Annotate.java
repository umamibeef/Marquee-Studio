import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.data.*;
import ghidra.program.model.mem.*;
import java.io.*;
import java.util.*;

public class Annotate extends GhidraScript {
    public void run() throws Exception {
        String outdir = getScriptArgs()[0];
        // 1. Name key routines we reverse-engineered by hand.
        String[][] names = {
            {"00033468","comms_rx_byte_statemachine"},
            {"00033a08","comms_frame_handler"},
            {"000331a4","comms_tx_frame"},
            {"00033414","comms_tx_expected_name"},
            {"00033910","build_expected_lfs_name"},
            {"000341d0","dl_reset_state"},
            {"0003413c","comms_tx_raw"},
            {"00055838","timer_20s_setup"},
        };
        SymbolTable st = currentProgram.getSymbolTable();
        AddressFactory af = currentProgram.getAddressFactory();
        for (String[] nm : names) {
            try {
                Address a = af.getDefaultAddressSpace().getAddress(Long.parseLong(nm[0], 16));
                createLabel(a, nm[1], true, SourceType.USER_DEFINED);
            } catch (Exception e) { println("label fail " + nm[0] + ": " + e); }
        }
        // 2. String xref index: string -> referencing functions.
        PrintWriter xw = new PrintWriter(new FileWriter(outdir + "/string_xref.txt"));
        DataIterator di = currentProgram.getListing().getDefinedData(true);
        FunctionManager fm = currentProgram.getFunctionManager();
        ReferenceManager rm = currentProgram.getReferenceManager();
        int ns = 0;
        while (di.hasNext() && !monitor.isCancelled()) {
            Data d = di.next();
            if (d.getDataType() instanceof StringDataType || (d.getValue() != null && d.hasStringValue())) {
                String s = d.getDefaultValueRepresentation();
                if (s == null || s.length() < 4) continue;
                ns++;
                Set<String> fns = new TreeSet<>();
                for (Reference r : rm.getReferencesTo(d.getAddress())) {
                    Function f = fm.getFunctionContaining(r.getFromAddress());
                    if (f != null) fns.add(f.getName());
                }
                if (!fns.isEmpty())
                    xw.println(d.getAddress() + "  " + s + "  <- " + String.join(", ", fns));
            }
        }
        xw.close();
        println("string xref: " + ns + " strings");
        // 3. Focused decompile of the comms functions + callers.
        DecompInterface dc = new DecompInterface(); dc.openProgram(currentProgram);
        PrintWriter cw = new PrintWriter(new FileWriter(outdir + "/comms.c"));
        Set<Function> want = new LinkedHashSet<>();
        for (String[] nm : names) {
            Address a = af.getDefaultAddressSpace().getAddress(Long.parseLong(nm[0], 16));
            Function f = fm.getFunctionContaining(a);
            if (f != null) { want.add(f); for (Function c : f.getCallingFunctions(monitor)) want.add(c); }
        }
        for (Function f : want) {
            DecompileResults r = dc.decompileFunction(f, 90, monitor);
            cw.println("/* === " + f.getEntryPoint() + "  " + f.getName() + " === */");
            if (r != null && r.decompileCompleted()) cw.println(r.getDecompiledFunction().getC());
            cw.println();
        }
        cw.close();
        println("DONE comms.c: " + want.size() + " functions");
    }
}
