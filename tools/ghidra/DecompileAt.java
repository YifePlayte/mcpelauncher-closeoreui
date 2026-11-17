// Create a function at an address (if needed), decompile it, and report the route
// strings and callers. Works on a project imported with -noanalysis.
//
// @category CloseOreUI
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;

import java.util.*;

public class DecompileAt extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        long addrVal = Long.parseUnsignedLong(args[0].replace("0x", ""), 16);
        int maxChars = args.length > 1 ? Integer.parseInt(args[1]) : 4000;
        Address addr = toAddr(addrVal);

        // The project was imported with -noanalysis, so nothing is disassembled.
        // Force a bounded disassembly around the target before touching function
        // definitions, otherwise bodies are single-instruction stubs.
        clearListing(addr, addr.add(16000));
        disassemble(addr);

        Function fn = getFunctionContaining(addr);
        if (fn == null) {
            println("[..] creating function at " + addr);
            CreateFunctionCmd cmd = new CreateFunctionCmd(addr);
            cmd.applyTo(currentProgram, monitor);
            fn = getFunctionAt(addr);
        }
        if (fn == null) {
            println("[!] could not create function");
            return;
        }

        println("=== " + fn.getName() + " @ " + fn.getEntryPoint());
        println("    entry_matches=" + fn.getEntryPoint().equals(addr));
        println("    body=" + fn.getBody().getMinAddress() + ".." + fn.getBody().getMaxAddress());
        println("    paramCount=" + fn.getParameterCount());

        // Disassemble forward a bounded window so references exist.
        Instruction insn = getInstructionAt(fn.getEntryPoint());
        if (insn == null) {
            disassemble(fn.getEntryPoint());
            insn = getInstructionAt(fn.getEntryPoint());
        }

        Set<String> routes = new LinkedHashSet<>();
        Set<String> otherStrings = new LinkedHashSet<>();
        int count = 0;
        Address limit = fn.getEntryPoint().add(12000);
        while (insn != null && insn.getAddress().compareTo(limit) < 0 && count < 4000) {
            for (Reference ref : insn.getReferencesFrom()) {
                String s = readStringAt(ref.getToAddress());
                if (s == null) continue;
                if (s.startsWith("/") && s.length() < 80 && s.matches("/[A-Za-z0-9_\\-./:]+")) {
                    routes.add(s);
                } else if (otherStrings.size() < 20) {
                    otherStrings.add(s);
                }
            }
            insn = insn.getNext();
            count++;
        }
        println("    insns walked: " + count);
        println("    route strings: " + routes.size());
        for (String r : routes) println("        " + r);
        if (!otherStrings.isEmpty()) {
            println("    other strings (sample):");
            for (String s : otherStrings) println("        " + s);
        }

        DecompInterface di = new DecompInterface();
        di.toggleCCode(true);
        di.openProgram(currentProgram);
        DecompileResults res = di.decompileFunction(fn, 180, monitor);
        if (res != null && res.decompileCompleted()) {
            String code = res.getDecompiledFunction().getC();
            println("--- decompiled ---");
            println(code.length() > maxChars ? code.substring(0, maxChars) + "\n... (truncated)" : code);
        } else {
            println("[!] decompile failed: " + (res == null ? "null" : res.getErrorMessage()));
        }
        di.dispose();
    }

    private String readStringAt(Address a) {
        if (a == null) return null;
        Memory mem = currentProgram.getMemory();
        MemoryBlock block = mem.getBlock(a);
        if (block == null || !block.isInitialized()) return null;
        try {
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < 160; i++) {
                byte b = mem.getByte(a.add(i));
                if (b == 0) break;
                if (b < 0x20 || b > 0x7E) return null;
                sb.append((char) b);
            }
            return sb.length() >= 3 ? sb.toString() : null;
        } catch (Exception e) {
            return null;
        }
    }
}
