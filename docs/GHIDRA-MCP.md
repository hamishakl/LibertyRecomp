# Ghidra + ghidra-mcp for guest-side work

Ghidra gives a decompiled view, cross-references and struct-aware offsets of the recompiled
Xbox 360 code, which is what the address-resolution and hook work (US↔PAL function matching,
vtable hunts, data globals, TU5 hook review) lacks today. `ghidra-mcp` exposes that analysis to
Claude Code over MCP so the questions can be asked from the session instead of through the GUI.
It is a research aid, not a route to a source port: the fork stays a static recompilation.

Use it for guest-side work only. Renderer, presenter and input code is ours and gains nothing.

## 1. Produce the images Ghidra will analyse

Ghidra has no built-in XEX support, and the retail XEX is encrypted and compressed. The repo already
has the tooling to turn it into a plain memory image; the same image is what codegen works from.

```bash
cd ~/repos/personal/LibertyRecomp
pip install cryptography                      # once; extract_basefile.py and apply_tu.py need it
M=glue/rexglue-sdk-main/thirdparty/libmspack/libmspack/mspack
cc -O2 -w -I$M tools/xex/lzxdelta.c $M/lzxd.c $M/system.c -o /tmp/lzxdelta

A=glue/rexglue-sdk-main/gta4-recomp
mkdir -p local/ghidra
# PAL disc executable, decrypted and decompressed (MZ header, loads at 0x82000000)
python3 tools/xex/extract_basefile.py $A/assets_pal/default.xex local/ghidra/pal_base.bin
# PAL + Title Update 5 (the build the fork actually runs)
python3 tools/xex/apply_tu.py $A/assets_pal/default.xex local/ghidra/pal_base.bin \
    $A/assets_pal/default.xexp /tmp/lzxdelta local/ghidra/pal_tu5.bin
# US v8 executable, for side-by-side matching against the upstream hooks (only if that dump is
# present at assets/default_v8.xex; it was not on this machine on 2026-10-10)
python3 tools/xex/extract_basefile.py $A/assets/default_v8.xex local/ghidra/us_v8.bin
```

`apply_tu.py` accepts either an STFS title-update package or a bare `default.xexp`. `local/` is
gitignored; these images are game code and must not be committed.

Done on 2026-10-10: `local/ghidra/pal_base.bin` (0x11F0000 bytes) and `local/ghidra/pal_tu5.bin`
(0x1300000 bytes, 44 patch blocks SHA-1 verified) exist, so start at step 2.

## 2. Install Java, Ghidra, uv, Maven, Gradle (DONE 2026-10-10)

```bash
brew install --cask temurin@21                # Ghidra 12.x needs Java 21
brew install maven gradle                     # ghidra-mcp build, and Ghidra's native build below
# ghidra-mcp pins Ghidra 12.1.4 (pom.xml ghidra.version; same major.minor required). Download that
# release zip, not the Homebrew cask:
gh release download Ghidra_12.1.4_build -R NationalSecurityAgency/ghidra -p "ghidra_12.1.4_PUBLIC*.zip" -D ~/ghidra
cd ~/ghidra && unzip -q ghidra_12.1.4_PUBLIC_*.zip && xattr -dr com.apple.quarantine ghidra_12.1.4_PUBLIC
# uv is at /opt/homebrew/bin/uv
```

⚠ **The public 12.1.4 zip ships native binaries for Linux and Windows only.** Without them every
decompile fails with `os/mac_arm_64/decompile does not exist`. Build them (a few seconds with the
Command Line Tools present), and copy them beside the shipped platforms, since the build lands in
`build/os` and the installed launcher did not find them there:

```bash
cd ~/ghidra/ghidra_12.1.4_PUBLIC/support/gradle
JAVA_HOME=$(/usr/libexec/java_home -v 21) gradle --no-daemon buildNatives   # Homebrew gradle defaults to JDK 27; pin 21
for m in Decompiler DemanglerGnu FileFormats; do d=../../Ghidra/Features/$m; [ -d $d/build/os/mac_arm_64 ] && mkdir -p $d/os && cp -R $d/build/os/mac_arm_64 $d/os/; done
```

The 570 MB zip plus 1 GB install plus the Maven/Gradle caches need about 3 GB free. The disk was
at 141 MB free when this was first attempted; `docs/BACKLOG.md` is not the place for that, but
check `df -h /System/Volumes/Data` first.

## 3. Build the analysed project (DONE 2026-10-10, repeatable)

`tools/ghidra/analyze_pal.sh` does all of this: imports `local/ghidra/pal_tu5.bin` as a raw binary
at `0x82000000` with language `PowerPC:BE:64:A2ALT-32addr` (see the warning above), runs `SeedLibertySymbols.java` to create a
function at every one of the **38,070 recompiled PAL function addresses** (taken from the address
table in `generated_pal/gta4_init.cpp` by `make_symbols.py`) plus the 19 resolved data globals from
`manual_map.json` as labels (`dat_pal_<PAL>_us_<US>`), then auto-analyses and prints the counts.

Result on 2026-10-10: project `~/ghidra/projects/GTAIV_PAL_TU5.gpr`, program `pal_tu5.bin`,
**38,395 functions, 1.82M instructions, 115k defined data**, analysis 76 s. Names match every
`sub_82......` in `generated_pal/`, `gta4_pal_config.toml` and `manual_map.json`. Verified through
the MCP: `sub_8236C750` (radar render phase, vtable slot 4) decompiles to 52 lines and shows the
`+0x8E8 = 7` store the PAL vtable hunt was based on; `0x82543B28` resolves to `sub_82543B20`.

⚠ **The seed script disables the "Non-Returning Functions - Discovered" analyzer.** With it on,
the register save/restore helper at `829FF3C8` (a `bl` target in nearly every function, outside the
recompiled table) was marked noreturn and 2,362 callers decompiled to a single call; instruction
count was 946k instead of 1.82M. Two earlier attempts failed that way, one of them also from using
the 32-bit language.

- Without the seed, a raw import has no entry points and the analysis finds almost nothing. Always
  seed first.
- The recompiled address table contains some branch-target fragments as well as true functions;
  about 4,000 seeded entries end up merged into their parent's body rather than as separate
  Ghidra functions. If a `sub_` name from the fork is missing, query by address instead.
- An `XEXLoaderWV` import (github.com/zeroKilo/XEXLoaderWV) would recover PE sections and `.pdata`
  but was not needed; the raw image plus seed gives the same function set the fork uses.
- The US v8 executable is not on this machine (`assets/default_v8.xex`), so there is no US project;
  the US side of any comparison comes from upstream's committed `generated/` C++ as before.

## 4. ghidra-mcp (DONE 2026-10-10)

```bash
git clone https://github.com/bethington/ghidra-mcp ~/repos/tools/ghidra-mcp   # v7.0.0, pins Ghidra 12.1.4
cd ~/repos/tools/ghidra-mcp
GH=~/ghidra/ghidra_12.1.4_PUBLIC
python3 -m tools.setup ensure-prereqs --ghidra-path "$GH"
python3 -m tools.setup build                      # Maven, ~15 s
python3 -m tools.setup deploy --ghidra-path "$GH" # installs to ~/Library/ghidra/ghidra_12.1.4_PUBLIC/Extensions/GhidraMCP
```

`deploy` also **launches the Ghidra GUI** with the plugin enabled and no project. Quit that one
and start Ghidra on the project instead; the plugin listens on `http://127.0.0.1:8089/`:

```bash
~/ghidra/ghidra_12.1.4_PUBLIC/ghidraRun ~/ghidra/projects/GTAIV_PAL_TU5.gpr &
curl http://127.0.0.1:8089/check_connection                                 # {"status":"ok","server_kind":"gui",...}
curl -X POST http://127.0.0.1:8089/open_program -H 'Content-Type: application/json' -d '{"path":"/pal_tu5.bin"}'
```

Registered with Claude Code at user scope (`claude mcp list` shows `ghidra-mcp ... Connected`):

```bash
claude mcp add-json ghidra-mcp --scope user '{
  "command": "/opt/homebrew/bin/uv",
  "args": ["run", "--directory", "/Users/ham/repos/tools/ghidra-mcp", "bridge-mcp-ghidra", "--transport", "stdio"],
  "env": { "GHIDRA_MCP_URL": "http://127.0.0.1:8089" }
}'
```

Ghidra must be running with the program open whenever the MCP is used; the bridge only relays to
the plugin's port. Start a session with the `ghidraRun` line above.

Route names worth knowing when poking the plugin directly with curl (the MCP tools wrap the same):
`/get_functions?name=sub_82543AA0&fields=signature,decompiled_code,callers,xrefs,disassembly`
(the field is `decompiled_code`, there is no `/decompile` route), `/find_functions`,
`/list_open_programs`, `/list_project_files`.

## Working notes for the session

- Ghidra reopens `pal_tu5.bin` by itself when launched on the project, so `/check_connection`
  already reports the program; the `open_program` POST is then a no-op.
- Useful raw routes (all GET): `/get_functions?name=sub_X&fields=signature,decompiled_code,callers`
  returns a flat object (`decompiled_code`, `size`, `callers[]`), `/read_memory?address=820B0274&length=32`
  (hex + bytes), `/disassemble_bytes?start_address=82A3BA80&length=16`.
- VMX128-heavy bodies truncate: `sub_822720B0` and `sub_823A8E28` decompile to 20 bytes with
  "bad instruction data". For those, the generated C++ comments are the source.
- Done 2026-10-10: the TU5-changed hook review (`tools/xex/review_changed_hooks.py`, results in
  `docs/PAL-PORT.md`). Ghidra was useful for the decompiled view of what a changed hunk does
  (e.g. the dropped probe in world activation) and for reading data (`0x820B0274` viewport);
  the hunk classification itself comes from the generated trees.
- Next tasks worth the setup: confirming the remaining `unresolved` entries in `manual_map.json`,
  and any new gameplay hook.
- Keep `manual_map.json` and `gta4_pal_config.toml` as the source of truth. Ghidra is where the
  reading happens; the fork's files are where the result is recorded.
- The generated C++ keeps the raw PPC in comments, which is still the fastest way to confirm a
  single instruction. Ghidra earns its keep on control flow, xrefs and struct offsets.
