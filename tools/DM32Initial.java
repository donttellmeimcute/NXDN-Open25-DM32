// Static research exporter. Does not contact a radio.
// Run with the pinned C-SKY v2 language module; verify decoding independently.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import java.io.*;

public class DM32Initial extends GhidraScript {
    public void run() throws Exception {
        String outDir = getScriptArgs()[0];
        Address start = currentProgram.getMinAddress();
        disassemble(start);
        Function entry = createFunction(start, "application_entry_candidate");
        try (PrintWriter out = new PrintWriter(new File(outDir, "startup-ghidra.txt"))) {
            out.println("Program: " + currentProgram.getName());
            out.println("Language: " + currentProgram.getLanguageID());
            out.println("Image base: " + currentProgram.getImageBase());
            InstructionIterator it = currentProgram.getListing().getInstructions(start, true);
            int n = 0;
            while (it.hasNext() && n++ < 180) {
                Instruction i = it.next();
                out.printf("%s  %s%n", i.getAddress(), i);
            }
            if (entry != null) {
                DecompInterface dec = new DecompInterface();
                dec.openProgram(currentProgram);
                DecompileResults result = dec.decompileFunction(entry, 30, monitor);
                out.println("\nCandidate decompilation (module semantics unverified):");
                if (result.decompileCompleted()) out.println(result.getDecompiledFunction().getC());
                else out.println(result.getErrorMessage());
                dec.dispose();
            }
        }
    }
}
