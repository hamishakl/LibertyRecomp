// Seed a LibertyRecomp image with the fork's known symbols before auto-analysis.
// Args: path to a text file of lines "name 0xADDRESS f|l" (f = function entry, l = data label).
//@category LibertyRecomp
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.symbol.*;
import ghidra.program.model.listing.*;
import java.nio.file.*;
import java.util.*;

public class SeedLibertySymbols extends GhidraScript {
  @Override
  public void run() throws Exception {
    String[] args = getScriptArgs();
    if (args.length < 1) { printerr("usage: SeedLibertySymbols <symbols.txt>"); return; }
    List<String> lines = Files.readAllLines(Paths.get(args[0]));
    AddressSet entries = new AddressSet();
    int labels = 0, functions = 0;
    monitor.initialize(lines.size());
    for (String line : lines) {
      monitor.checkCancelled(); monitor.incrementProgress(1);
      String[] f = line.trim().split("\\s+");
      if (f.length < 2 || f[0].isEmpty()) continue;
      Address a = toAddr(Long.parseUnsignedLong(f[1].replace("0x", ""), 16));
      if (!currentProgram.getMemory().contains(a)) continue;
      String kind = f.length > 2 ? f[2] : "l";
      if (kind.equals("f")) entries.add(a);
      else { createLabel(a, f[0], true, SourceType.IMPORTED); labels++; }
    }
    // The Xenon register save/restore helpers (__savegprlr_N and friends) are bl targets that are
    // not in the recompiled function table. If one is left undisassembled, the "Non-Returning
    // Functions - Discovered" analyzer marks it noreturn and truncates every caller at that bl.
    setAnalysisOption(currentProgram, "Non-Returning Functions - Discovered", "false");
    println("disassembling from " + entries.getNumAddresses() + " entry points");
    new DisassembleCommand(entries, null, true).applyTo(currentProgram, monitor);
    Listing listing = currentProgram.getListing();
    ReferenceManager refs = currentProgram.getReferenceManager();
    int helpers = 0;
    for (int pass = 0; pass < 4; pass++) {
      AddressSet missing = new AddressSet();
      var it = refs.getReferenceDestinationIterator(currentProgram.getMemory(), true);
      while (it.hasNext()) {
        monitor.checkCancelled();
        Address d = it.next();
        if (listing.getInstructionAt(d) != null) continue;
        for (Reference r : refs.getReferencesTo(d)) if (r.getReferenceType().isCall()) { missing.add(d); break; }
      }
      if (missing.isEmpty()) break;
      new DisassembleCommand(missing, null, true).applyTo(currentProgram, monitor);
      for (Address d : missing.getAddresses(true)) {
        Instruction in = listing.getInstructionAt(d);
        if (in == null) continue;
        if (getFunctionAt(d) == null) new CreateFunctionCmd(null, d, null, SourceType.ANALYSIS).applyTo(currentProgram, monitor);
        String name = HelperName(in);
        if (name != null && getFunctionAt(d) != null) getFunctionAt(d).setName(name, SourceType.ANALYSIS);
        helpers++;
      }
    }
    // Flow-following usually reaches the helpers already; name whatever landed there.
    for (Function fn : listing.getFunctions(true)) {
      if (!fn.getName().startsWith("FUN_")) continue;
      Instruction in = listing.getInstructionAt(fn.getEntryPoint());
      String name = in == null ? null : HelperName(in);
      if (name != null) { fn.setName(name, SourceType.ANALYSIS); helpers++; }
    }
    println("disassembled or named " + helpers + " call targets outside the function table");
    for (String line : lines) {
      monitor.checkCancelled();
      String[] f = line.trim().split("\\s+");
      if (f.length < 3 || !f[2].equals("f")) continue;
      Address a = toAddr(Long.parseUnsignedLong(f[1].replace("0x", ""), 16));
      if (!currentProgram.getMemory().contains(a)) continue;
      if (getFunctionAt(a) == null) {
        if (new CreateFunctionCmd(f[0], a, null, SourceType.IMPORTED).applyTo(currentProgram, monitor)) functions++;
        else createLabel(a, f[0], true, SourceType.IMPORTED);
      } else getFunctionAt(a).setName(f[0], SourceType.IMPORTED);
    }
    println("seeded " + functions + " functions, " + labels + " labels");
  }

  // Name the entries of the Xenon save/restore chains from their first instruction.
  private static String HelperName(Instruction in) {
    String m = in.getMnemonicString(), op = in.getDefaultOperandRepresentation(0);
    if (op == null || op.length() < 2) return null;
    String n = op.replaceAll("[^0-9]", "");
    if (n.isEmpty()) return null;
    switch (m) {
      case "std": return op.startsWith("r") ? "__savegprlr_" + n : null;
      case "ld": return op.startsWith("r") ? "__restgprlr_" + n : null;
      case "stfd": return "__savefpr_" + n;
      case "lfd": return "__restfpr_" + n;
      default: return null;
    }
  }
}
