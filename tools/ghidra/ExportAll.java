// Headless export of every function of the Dracula executable: list + decompiled C.
// args: <outdir>
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.lang.*;
import java.io.*;
import java.math.BigInteger;

public class ExportAll extends GhidraScript {
    @Override
    public void run() throws Exception {
        String out = getScriptArgs()[0];
        new File(out).mkdirs();
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        PrintWriter list = new PrintWriter(new FileWriter(out + "/functions.tsv"));
        PrintWriter all = new PrintWriter(new FileWriter(out + "/decomp.c"));
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        int n = 0;
        while (it.hasNext() && !monitor.isCancelled()) {
            Function f = it.next();
            AddressSetView b = f.getBody();
            list.printf("%s\t%s\t%d\t%s%n", f.getEntryPoint(), b.getMaxAddress(), b.getNumAddresses(), f.getName());
            DecompileResults r = di.decompileFunction(f, 120, monitor);
            all.printf("// ==== %s %s ====%n", f.getEntryPoint(), f.getName());
            if (r != null && r.decompileCompleted()) all.println(r.getDecompiledFunction().getC());
            else all.println("// decompile failed");
            n++;
        }
        list.close(); all.close();
        println("exported " + n + " functions");
    }
}
