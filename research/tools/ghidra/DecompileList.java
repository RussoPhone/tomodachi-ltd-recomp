// Decompile a list of functions (module offsets, one per line, optional "name" after a tab) to C files.
// Headless usage (no auto-analysis needed: function starts and names come from the SWITCHPILER ELF):
//   analyzeHeadless <proj> <name> -process main.elf -noanalysis -scriptPath scripts/ghidra \
//       -postScript DecompileList.java <list.tsv> <out_dir>
// @category SWITCHPILER
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Symbol;
import java.io.File;
import java.io.PrintWriter;
import java.nio.file.Files;
import java.util.List;

public class DecompileList extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        List<String> lines = Files.readAllLines(new File(args[0]).toPath());
        File out = new File(args[1]);
        out.mkdirs();
        long base = currentProgram.getImageBase().getOffset();
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        int ok = 0, failed = 0;
        for (String line : lines) {
            line = line.trim();
            if (line.isEmpty() || line.startsWith("#")) continue;
            String[] parts = line.split("\t");
            long off = Long.decode(parts[0]);
            Address addr = toAddr(base + off);
            Function fn = getFunctionAt(addr);
            if (fn == null) {
                fn = createFunction(addr, null);
            }
            if (fn == null) {
                println("could not create function at " + addr);
                failed++;
                continue;
            }
            DecompileResults res = ifc.decompileFunction(fn, 120, monitor);
            String name = fn.getName();
            Symbol sym = getSymbolAt(addr);
            if (sym != null) name = sym.getName();
            String file = String.format("%s_%x.c", name.replaceAll("[^A-Za-z0-9_]", "_"), off);
            try (PrintWriter pw = new PrintWriter(new File(out, file))) {
                pw.printf("// %s @ module+0x%x (size %d) - Ghidra pseudo-C of machine-translated game code, not original source%n",
                          name, off, fn.getBody().getNumAddresses());
                if (parts.length > 1) pw.printf("// context: %s%n", parts[1]);
                if (res != null && res.decompileCompleted()) {
                    pw.print(res.getDecompiledFunction().getC());
                    ok++;
                } else {
                    pw.printf("// decompilation failed: %s%n", res == null ? "null" : res.getErrorMessage());
                    failed++;
                }
            }
        }
        ifc.dispose();
        println("DecompileList: " + ok + " decompiled, " + failed + " failed -> " + out);
    }
}
