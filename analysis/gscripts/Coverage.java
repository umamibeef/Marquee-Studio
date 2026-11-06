import ghidra.app.script.GhidraScript;
import ghidra.program.model.mem.*;
import ghidra.program.model.listing.*;
public class Coverage extends GhidraScript {
    public void run() throws Exception {
        Listing l = currentProgram.getListing();
        long code=0, data=0;
        AddressSetView exec = currentProgram.getMemory().getExecuteSet();
        code = l.getNumInstructions();
        long undef = 0;
        var it = l.getUndefinedData(exec, true);
        while(it.hasNext()&&!monitor.isCancelled()){ it.next(); undef++; }
        println("instructions="+code+" undefined_bytes_in_exec="+undef+" functions="+currentProgram.getFunctionManager().getFunctionCount());
    }
}
