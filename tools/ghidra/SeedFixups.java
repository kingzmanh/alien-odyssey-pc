// Pre-analysis: disassemble at every code-object fixup target so Ghidra
// sees the code reached only through pointer tables. args: <fixup_targets.txt>
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.program.model.address.*;
import java.io.*;

public class SeedFixups extends GhidraScript {
    @Override
    public void run() throws Exception {
        BufferedReader r = new BufferedReader(new FileReader(getScriptArgs()[0]));
        String line; int n = 0;
        AddressSpace sp = currentProgram.getAddressFactory().getDefaultAddressSpace();
        while ((line = r.readLine()) != null) {
            line = line.trim(); if (line.isEmpty()) continue;
            Address a = sp.getAddress(Long.parseLong(line, 16));
            if (currentProgram.getListing().getInstructionAt(a) == null) {
                new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
                n++;
            }
        }
        r.close();
        println("seeded " + n);
    }
}
