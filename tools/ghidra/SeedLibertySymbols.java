// Seed a LibertyRecomp image with the fork's known symbols before auto-analysis.
// Args: path to a text file of lines "name 0xADDRESS f|l" (f = function entry, l = data label).
//@category LibertyRecomp
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.symbol.SourceType;
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
    println("disassembling from " + entries.getNumAddresses() + " entry points");
    new DisassembleCommand(entries, null, true).applyTo(currentProgram, monitor);
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
}
