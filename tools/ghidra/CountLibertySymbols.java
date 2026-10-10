//@category LibertyRecomp
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
public class CountLibertySymbols extends GhidraScript {
  @Override public void run() throws Exception {
    FunctionManager fm = currentProgram.getFunctionManager();
    long instr = currentProgram.getListing().getNumInstructions();
    Function f = fm.getFunctionAt(toAddr(0x8236C140L));
    println("functions=" + fm.getFunctionCount() + " instructions=" + instr + " defined-data=" + currentProgram.getListing().getNumDefinedData());
    println("sub_8236C140 (PAL radar render phase ctor) = " + (f == null ? "missing" : f.getName() + " body=" + f.getBody().getNumAddresses()));
  }
}
