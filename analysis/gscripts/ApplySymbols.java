// Apply analysis/symbols.tsv to the current program: rename functions, label data, and attach
// each line's comment as a plate comment, so decompiled output reads with our names.
// Args: <symbols.tsv>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.data.*;
import java.io.*;
import java.util.*;

public class ApplySymbols extends GhidraScript {
    public void run() throws Exception {
        String path = getScriptArgs()[0];
        int funcs = 0, data = 0, created = 0, bad = 0, types = 0, locals = 0;
        DataTypeManager dtm = currentProgram.getDataTypeManager();
        CategoryPath cat = new CategoryPath("/marquee");
        Map<String, DataType> known = new HashMap<>();
        List<String[]> prefixes = new ArrayList<>();
        known.put("u8", ByteDataType.dataType);
        known.put("u16", WordDataType.dataType);
        known.put("u32", DWordDataType.dataType);
        known.put("ptr", PointerDataType.dataType);
        known.put("i8", SignedByteDataType.dataType);
        known.put("i16", ShortDataType.dataType);
        known.put("i32", IntegerDataType.dataType);
        known.put("void", VoidDataType.dataType);
        known.put("u64", QWordDataType.dataType);
        known.put("i64", LongLongDataType.dataType);
        known.put("char", CharDataType.dataType);
        // "fn": a function taking and returning nothing, so "fn*" types a callback pointer that the code calls
        FunctionDefinitionDataType fnd = new FunctionDefinitionDataType(cat, "void_fn", dtm);
        fnd.setReturnType(VoidDataType.dataType);
        known.put("fn", dtm.addDataType(fnd, DataTypeConflictHandler.REPLACE_HANDLER));
        Listing listing = currentProgram.getListing();
        // symbols.tsv is the single source of truth: first drop user-given function names that are
        // no longer in it (prefix tags are re-applied below)
        Set<Long> inFile = new HashSet<>();
        try (BufferedReader r0 = new BufferedReader(new FileReader(path))) {
            String l0;
            while ((l0 = r0.readLine()) != null) {
                String[] c0 = l0.split("\t");
                if (c0.length >= 3 && c0[0].matches("[0-9a-f]{8}")) inFile.add(Long.parseLong(c0[0], 16));
            }
        }
        int reset = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (f.getSymbol().getSource() == SourceType.USER_DEFINED
                    && !inFile.contains(f.getEntryPoint().getOffset())) {
                f.setName(null, SourceType.DEFAULT);
                reset++;
            }
        }
        println("ApplySymbols: " + reset + " stale names reset");
        // two passes: regions and structs first, so a type can be used above its definition
        List<String> lines = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new FileReader(path))) {
            String l;
            while ((l = r.readLine()) != null) lines.add(l);
        }
        for (int pass = 0; pass < 2; pass++) {
            for (String line : lines) {
                if (line.isBlank() || line.startsWith("#")) continue;
                String[] c = line.split("\t");
                boolean early = c[0].equals("region") || c[0].equals("struct");
                if (early != (pass == 0)) continue;
                if (c[0].equals("prefix")) {                       // prefix <p> <start> <end> <comment>
                    prefixes.add(c);
                    continue;
                }
                if (c[0].equals("region")) {                       // region <name> <start> <end>
                    Address st = toAddr(Long.parseLong(c[2], 16));
                    long len = Long.parseLong(c[3], 16) - Long.parseLong(c[2], 16);
                    if (currentProgram.getMemory().getBlock(st) == null) {
                        currentProgram.getMemory().createUninitializedBlock(c[1], st, len, false);
                        println("created region " + c[1]);
                    }
                    continue;
                }
                if (c[0].equals("struct")) {                       // struct <name> <field:type,...>
                    StructureDataType st = new StructureDataType(cat, c[1], 0, dtm);
                    for (String fld : c[2].split(",")) {
                        String[] ft = fld.split(":");
                        st.add(resolve(known, ft[1]), ft[0], null);
                    }
                    DataType dt = dtm.addDataType(st, DataTypeConflictHandler.REPLACE_HANDLER);
                    known.put(c[1], dt);
                    types++;
                    continue;
                }
                if (c[0].equals("stack")) {                        // stack <func> <offset> <name> <type> <comment>
                    try {                                          // one local over what Ghidra split apart
                        Function f = getFunctionAt(toAddr(Long.parseLong(c[1], 16)));
                        int off = (int) Long.decode(c[2]).longValue();
                        DataType dt = resolve(known, c[4]);
                        StackFrame sf = f.getStackFrame();
                        for (Variable v : sf.getStackVariables()) {
                            int vo = v.getStackOffset(), vl = Math.max(1, v.getLength());
                            if (vo < off + dt.getLength() && off < vo + vl) sf.clearVariable(vo);
                        }
                        Variable v = sf.createVariable(c[3], off, dt, SourceType.USER_DEFINED);
                        if (c.length > 5) v.setComment(c[5]);
                        locals++;
                    } catch (Exception e) { println("fail stack " + line + ": " + e); bad++; }
                    continue;
                }
                if (c.length < 5) { println("skip: " + line); bad++; continue; }
                Address a = toAddr(Long.parseLong(c[0], 16));
                String name = c[3].equals("probable") && !c[2].endsWith("_maybe") ? c[2] + "_maybe" : c[2];
                String note = (c[3].equals("probable") ? "[probable] " : "") + c[4];
                try {
                    if (c[1].equals("func")) {
                        Function f = getFunctionAt(a);
                        if (f == null) { f = createFunction(a, name); created++; }
                        if (f == null) { println("no function at " + c[0]); bad++; continue; }
                        f.setName(name, SourceType.USER_DEFINED);
                        f.setComment(note);
                        if (c.length >= 6 && !c[5].isEmpty()) setSignature(f, known, c[5]);
                        funcs++;
                    } else {
                        createLabel(a, name, true, SourceType.USER_DEFINED);
                        listing.setComment(a, CodeUnit.PLATE_COMMENT, name + ": " + note);
                        if (c.length >= 6) {                       // typed data, e.g. frame_rec[8]
                            DataType dt = resolve(known, c[5]);
                            listing.clearCodeUnits(a, a.add(dt.getLength() - 1), false);
                            listing.createData(a, dt);
                        }
                        data++;
                    }
                } catch (Exception e) { println("fail " + c[0] + " " + name + ": " + e); bad++; }
            }
        }
        // provenance prefixes, after real names so those always win
        int tagged = 0;
        for (String[] p : prefixes) {
            long lo = Long.parseLong(p[2], 16), hi = Long.parseLong(p[3], 16);
            for (Function f : currentProgram.getFunctionManager().getFunctions(toAddr(lo), true)) {
                long a = f.getEntryPoint().getOffset();
                if (a >= hi) break;
                if (!f.getName().startsWith("FUN_") && !f.getName().startsWith(p[1] + "_")) continue;
                f.setName(p[1] + "_" + String.format("%08x", a), SourceType.USER_DEFINED);
                if (f.getComment() == null) f.setComment(p[4]);
                tagged++;
            }
        }
        println("ApplySymbols: " + tagged + " functions tagged by provenance");
        println("ApplySymbols: " + types + " structs, " + funcs + " functions (" + created + " created), " + data + " data labels, " + locals + " stack locals, " + bad + " problems");
    }

    /** Signature column of a func line: "ret;name:type,name:type" (ret "-" keeps the return type,
     *  params "?" keeps the parameters, "..." makes it varargs).
     *  Types as in resolve(), plus a trailing '*' for pointers. */
    private void setSignature(Function f, Map<String, DataType> known, String spec) throws Exception {
        String[] rp = spec.split(";", -1);
        DataType ret = rp[0].equals("-") ? null : resolve(known, rp[0]);
        if (rp.length > 1 && rp[1].equals("?")) {              // "ret;?" = return type only, keep parameters
            if (ret != null) f.setReturnType(ret, SourceType.USER_DEFINED);
            return;
        }
        List<Variable> ps = new ArrayList<>();
        boolean varargs = false;
        if (rp.length > 1 && !rp[1].isEmpty())
            for (String a : rp[1].split(",")) {
                if (a.equals("...")) { varargs = true; continue; }
                String[] nt = a.split(":");
                ps.add(new ParameterImpl(nt[0], resolve(known, nt[1]), currentProgram));
            }
        f.updateFunction(currentProgram.getCompilerSpec().getDefaultCallingConvention().getName(), ret == null ? null : new ReturnParameterImpl(ret, currentProgram), ps,
                Function.FunctionUpdateType.DYNAMIC_STORAGE_ALL_PARAMS, true, SourceType.USER_DEFINED);
        f.setVarArgs(varargs);
    }

    /** "u32", "ledpanel_frame", "font*", or "name[N]" arrays of any of these. */
    private DataType resolve(Map<String, DataType> known, String spec) {
        if (spec.endsWith("*")) return new PointerDataType(resolve(known, spec.substring(0, spec.length() - 1)));
        int br = spec.indexOf('[');
        if (br < 0) {
            DataType dt = known.get(spec);
            if (dt == null) throw new IllegalArgumentException("unknown type " + spec);
            return dt;
        }
        DataType el = resolve(known, spec.substring(0, br));
        int n = Integer.parseInt(spec.substring(br + 1, spec.length() - 1));
        return new ArrayDataType(el, n, el.getLength());
    }
}
