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

## 2. Install Java, Ghidra, uv

```bash
brew install --cask temurin@21                # Ghidra 12.x needs Java 21
# Ghidra: ghidra-mcp pins a Ghidra version (12.1.4 at the time of writing). Download that exact
# release zip from https://github.com/NationalSecurityAgency/ghidra/releases and unzip it to
# ~/ghidra/ghidra_12.1.4_PUBLIC. The Homebrew cask tracks a different version; avoid it here.
xattr -dr com.apple.quarantine ~/ghidra/ghidra_12.1.4_PUBLIC
# uv is already installed at /opt/homebrew/bin/uv
```

Run `~/ghidra/ghidra_12.1.4_PUBLIC/ghidraRun` once to confirm it starts, then quit.

## 3. Load the images

Two options. Try the loader first; fall back to the raw import if it fails on these files.

**Option A, XEX loader (preferred).** `XEXLoaderWV` (github.com/zeroKilo/XEXLoaderWV, or the
SaveEditors fork which ships ready-made zips on its Releases page) is a Ghidra extension that
reads XEX files directly, including `.xexp` delta patches, and recovers sections and `.pdata`
functions. Install the zip matching your Ghidra version via **File > Install Extensions**, restart,
then **File > Import File** on `assets_pal/default.xex`. If the loader offers to apply a patch,
point it at `default.xexp`.

**Option B, raw image.** **File > Import File** on `local/ghidra/pal_tu5.bin`, format **Raw Binary**,
language **PowerPC:BE:32:default** (the Xenon is a 64-bit core but all addresses are 32-bit; if
the decompiler shows 64-bit register noise, re-import as **PowerPC:BE:64:A2ALT-32addr**), base
address **0x82000000**. The file is an MZ/PE image, so the headers sit at the base and code starts
at the first section; the function addresses then match `tools/xex/manual_map.json`, the
`gta4_pal_config.toml` entries and every `sub_82......` name in `generated_pal/`.

Run auto-analysis with defaults. Expect an hour or more on the 20 MB image; let it finish before
using the MCP. Make a second project the same way for `us_v8.bin` (base 0x82000000 as well) so
US and PAL can be compared.

Seed the PAL project with what the fork already knows, so names match the code:
`tools/xex/manual_map.json` (code/data/unresolved) and the `[functions]` table in
`glue/rexglue-sdk-main/gta4-recomp/gta4_pal_config.toml` can be turned into Ghidra labels with a
short script once the MCP is up (rename_function / create_label calls), or imported through
Ghidra's **ImportSymbolsScript** from a `name address` text file.

## 4. Build and install ghidra-mcp

```bash
git clone https://github.com/bethington/ghidra-mcp ~/repos/tools/ghidra-mcp
cd ~/repos/tools/ghidra-mcp
GH=~/ghidra/ghidra_12.1.4_PUBLIC
python3 -m tools.setup ensure-prereqs --ghidra-path "$GH"   # wants Maven 3.9+; brew install maven if asked
python3 -m tools.setup build
python3 -m tools.setup deploy --ghidra-path "$GH"            # installs the extension into the user profile
# alternative without Maven: ./gradlew buildExtension -PGHIDRA_INSTALL_DIR="$GH", then
# File > Install Extensions > Add on the produced GhidraMCP-<version>.zip
```

In Ghidra: **File > Configure > Utility > Configure > GhidraMCPPlugin**, tick it, restart. With a
program open, the plugin listens on `http://127.0.0.1:8089/`:

```bash
curl http://127.0.0.1:8089/check_connection
```

## 5. Register the MCP server with Claude Code

User scope, so it is available in every project and nothing with local paths lands in the repo:

```bash
claude mcp add-json ghidra-mcp --scope user '{
  "command": "/opt/homebrew/bin/uv",
  "args": ["run", "--directory", "/Users/ham/repos/tools/ghidra-mcp", "bridge-mcp-ghidra", "--transport", "stdio"],
  "env": { "GHIDRA_MCP_URL": "http://127.0.0.1:8089" }
}'
claude mcp list                                 # should show ghidra-mcp
```

Ghidra must be running with the program open before the bridge is useful; the MCP only relays to
the plugin's HTTP port. Only one program is served at a time, so switch the open program in Ghidra
when moving between the US and PAL projects.

## Working notes for the session

- First tasks worth the setup: the TU5-changed hook review list (`docs/BACKLOG.md`), confirming the
  remaining `unresolved` entries in `manual_map.json`, and any new gameplay hook.
- Keep `manual_map.json` and `gta4_pal_config.toml` as the source of truth. Ghidra is where the
  reading happens; the fork's files are where the result is recorded.
- The generated C++ keeps the raw PPC in comments, which is still the fastest way to confirm a
  single instruction. Ghidra earns its keep on control flow, xrefs and struct offsets.
