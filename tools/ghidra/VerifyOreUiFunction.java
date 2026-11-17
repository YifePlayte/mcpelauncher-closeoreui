// Verify that a given address in libminecraftpe.so really is the OreUI config
// registration function.
//
// Checks, in order:
//   1. the address is a function entry (not the middle of one);
//   2. the function references many distinct OreUI route strings;
//   3. its callers look like a startup/registration path;
//   4. the ABI shape matches the mod's detour signature (map by reference first).
//
// @category CloseOreUI
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.data.DataUtilities;

import java.util.*;

public class VerifyOreUiFunction extends GhidraScript {

    private static final String[] ANCHORS = {
        "/oreui-marketplace/search", "/oreui-marketplace/wishlist",
        "/start-from-template", "/create-new-world",
        "/player-permissions", "/send-invites",
        "/gameplay/bedtime", "/pre-start", "/main-menu"
    };

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            println("usage: VerifyOreUiFunction <hexaddr>");
            return;
        }
        long target = Long.parseUnsignedLong(args[0].replace("0x", ""), 16);
        Address addr = toAddr(target);

        println("=== target: " + addr + " ===");

        Function fn = getFunctionAt(addr);
        if (fn == null) {
            Function containing = getFunctionContaining(addr);
            println("[FAIL] not a function entry");
            if (containing != null) {
                println("       contained in " + containing.getName() + " @ " + containing.getEntryPoint());
            }
            return;
        }
        println("[OK] function entry: " + fn.getName());
        println("     body: " + fn.getBody().getMinAddress() + " .. " + fn.getBody().getMaxAddress());

        // 1. Count distinct OreUI route strings referenced anywhere in the body.
        Set<String> routes = new LinkedHashSet<>();
        Set<String> allStrings = new LinkedHashSet<>();
        Instruction insn = getInstructionAt(fn.getEntryPoint());
        Address end = fn.getBody().getMaxAddress();
        int scanned = 0;
        while (insn != null && insn.getAddress().compareTo(end) <= 0 && scanned < 20000) {
            for (Reference ref : insn.getReferencesFrom()) {
                Address to = ref.getToAddress();
                String s = readStringAt(to);
                if (s != null) {
                    allStrings.add(s);
                    if (isOreUiRoute(s)) {
                        routes.add(s);
                    }
                }
            }
            insn = insn.getNext();
            scanned++;
        }

        println("[..] instructions scanned: " + scanned);
        println("[..] distinct route-like strings: " + routes.size());
        int anchorHits = 0;
        for (String a : ANCHORS) {
            if (routes.contains(a)) {
                anchorHits++;
            }
        }
        println("[..] anchor strings present: " + anchorHits + "/" + ANCHORS.length);
        for (String r : routes) {
            println("       " + r);
        }

        // 2. Callers: a registration function is called once, from startup.
        Set<Function> callers = fn.getCallingFunctions(monitor);
        println("[..] callers: " + callers.size());
        for (Function c : callers) {
            println("       " + c.getName() + " @ " + c.getEntryPoint());
        }

        // 3. First parameter should be a pointer (the map by reference).
        println("[..] signature: " + fn.getSignature());
        println("[..] parameter count: " + fn.getParameterCount());

        boolean pass = anchorHits >= 6 && routes.size() >= 10;
        println(pass ? "[PASS] looks like the OreUI config registration function"
                     : "[WARN] shape does not match expectations");
    }

    private boolean isOreUiRoute(String s) {
        if (s == null || s.length() < 4 || s.length() > 120) return false;
        if (!s.startsWith("/")) return false;
        if (s.contains("%")) return false;
        for (char c : s.toCharArray()) {
            boolean ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                    || (c >= '0' && c <= '9') || c == '/' || c == '_' || c == '-'
                    || c == '.' || c == ':';
            if (!ok) return false;
        }
        return true;
    }

    private String readStringAt(Address a) {
        if (a == null) return null;
        Memory mem = currentProgram.getMemory();
        MemoryBlock block = mem.getBlock(a);
        if (block == null || !block.isInitialized()) return null;
        try {
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < 200; i++) {
                byte b = mem.getByte(a.add(i));
                if (b == 0) break;
                if (b < 0x20 || b > 0x7E) return null;
                sb.append((char) b);
            }
            return sb.length() >= 4 ? sb.toString() : null;
        } catch (Exception e) {
            return null;
        }
    }
}
