import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import java.io.*;
public class BootDecomp extends GhidraScript {
  public void run() throws Exception {
    String out = getScriptArgs()[0];
    long[] ents = {0x100,0x128,0x8c0,0xc90,0xcb0,0xe50,0xe90,0x1240,0x1480,0x16c0,0x17ec,0x1870,0x1aa0,0x1b8c,0x1c18,0x1cf8,0x1ea8,0x2050,0x2138,0x2144,0x22cc,0x2320,0x250c,0x25fc};
    AddressSpace sp = currentProgram.getAddressFactory().getDefaultAddressSpace();
    for (long e : ents) {
      Address a = sp.getAddress(0xF8180000L + e);
      new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
      if (getFunctionAt(a) == null) new CreateFunctionCmd(a).applyTo(currentProgram, monitor);
    }
    DecompInterface di = new DecompInterface(); di.openProgram(currentProgram);
    PrintWriter cw = new PrintWriter(new FileWriter(out));
    int n=0, ok=0;
    for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
      n++; DecompileResults r = di.decompileFunction(f, 90, monitor);
      cw.println("/* === " + f.getEntryPoint() + "  " + f.getName() + "  (" + f.getBody().getNumAddresses() + " bytes) === */");
      if (r != null && r.decompileCompleted()) { cw.println(r.getDecompiledFunction().getC()); ok++; }
      else cw.println("// decompile failed");
    }
    cw.close(); println("BOOTDONE " + ok + "/" + n);
  }
}
