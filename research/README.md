# Research: decomp tools and the native graphics work

This folder is for contributors. Nothing here is needed to install or play, and the installer does not use it.
It holds the tools and patches from the original author's lab, published so others can continue.

**Rules that always apply:** never commit game files, keys, firmware, dumps, generated C, decompiled pseudo-C,
frame captures or shader dumps. Everything derived from the game stays in `local/` (gitignored). Generated or
decompiled code is a machine translation, never the game's original source.

## Layout

| Path | What it is |
|---|---|
| `tools/make-elf.py` | Writes a standard AArch64 ELF per module (rtld/main/sdk) with every known name, for Ghidra/IDA/radare2 |
| `tools/decomp-index.py` | One-pass call/reference index: direct calls, PLT imports, string and data references |
| `tools/data-index.py` | Reads the game's data archives (zstd, SARC, BYML) and its RSDB tables |
| `tools/link-data-code.py`, `recover-structs.py` | Finds the functions that read each data table and recovers C struct layouts from them |
| `tools/ptr-index.py` | Indexes every function pointer in data (vtables, callback tables) from the RELA/RELR relocations |
| `tools/name-classes.py` | Names C++ classes from the engine's own type-name virtuals, through their vtables |
| `tools/annotate-c.py` | Makes Ghidra pseudo-C readable (resolves `&UNK_71…` to strings, `sub_X` to known names) |
| `tools/find-const.py` | Finds the functions that build a given constant (hashes, magic numbers) |
| `tools/ghidra/DecompileList.java` | Headless Ghidra script: decompiles a list of offsets to `.c` files |
| `nvn-hle/patches/0009-0015` | Native graphics work for suyu (apply on top of `../patches/0001-0008`) |
| `tools/mkpatch.sh`, `verify-patches.sh` | Make a patch against pristine suyu + the previous patches; check the stack reproduces the tree |
| `tools/nvn-hle-run.sh`, `nvn-hle-gdb.sh` | Start the packaged build with the shadow renderer; same under gdb, with backtraces on a stall |
| `docs/native-graphics-notes.pt-BR.md` | Lab notes for the native graphics work (Portuguese): every finding and the exact open problem |

The scripts expect the lab layout they came from (`scripts/`, `local/`, `upstream/mk8-recomp` at the repository
root, `local/runtime.json` with the dump path). The installer's `scripts/lab.py` follows the same layout.

## Native graphics (NVN → Vulkan): where it stands

The game's code is already native x86-64. The Switch GPU is still emulated: the game's NVN driver writes Maxwell
command streams that suyu's `video_core` interprets. The goal is to answer NVN on the host with Vulkan instead.

1. **Patch 0009, trace:** hooks in the static dispatcher catch `nvnBootstrapLoader` / `nvnDeviceGetProcAddress` and
   give the game a fake address per NVN function, so every NVN call is seen and forwarded. Done.
2. **Patch 0010, shadow model:** every call is decoded into a host model (pools, buffers, textures, samplers,
   programs, command lists); small state objects are read with the driver's own layout. Done.
3. **Patches 0011-0015, shadow renderer:** our own Vulkan renderer runs next to the original and draws the recorded
   command lists off screen. It draws the game's scenes. **Open problem:** the final composition into the window
   texture. The composition draw (6 indices, recorded once and reused through `CallCommands`) is lost because the
   command-list handle mapping is wrong for some lists. Details and the next experiment are at the end of
   `docs/native-graphics-notes.pt-BR.md` ("Atualização 3" and "Atualização 4").
4. **Not started:** a hybrid mode (our renderer where everything in a frame is supported, emulated GPU otherwise),
   shipped as `play.sh --native-graphics`, then coverage of the whole game, then removing the emulated GPU.

Run the shadow renderer: `SUYU_NVN_HLE=shadow SUYU_NVN_BOOTSTRAP=nnSdk:0x502d10 ./tomodachi-native -g game/main`
(frames are dumped to `nvn-hle-frames/`; diagnostics go to stderr; keep LOG_* calls rare, suyu's logger stops when
flooded). `SUYU_DEBUG_RT_VA=<gpu va>` (patch 0014) prints the emulated GPU's draws into a render target, which is
the reference to compare against.
