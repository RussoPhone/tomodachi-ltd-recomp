# Issue drafts (to be opened on GitHub, then this file can be deleted)

## 1. Windows: complete install with a real game (steps 6-10)
Steps 1-5 pass on GitHub's Windows runner and two user-reported errors were fixed (winget discovery, Python < 3.12).
Steps 6-10 (dump, export window, clang-cl compile in `recomp_modules`, data extraction, package with `run.bat` and a
Start menu shortcut) have never completed on a real Windows PC. Run `install.bat` from a short path (e.g.
`C:\tomodachi`), and report the log the installer points to (`local\logs\`). Never attach keys or game files.

## 2. Native renderer: window composition is lost
In shadow mode (`research/README.md`) our Vulkan renderer draws the scenes, but the presented window texture never
receives the composition draw. Reference from the emulated GPU (`SUYU_DEBUG_RT_VA=0x501d80000`): an indexed draw of
6 indices, VS at GPU 0x400026000, FS at 0x400026400, every frame. That draw is recorded once in another command buffer
and called every frame with `nvnCommandBufferCallCommands`; our command-list handle mapping resolves the call to the
wrong list. Notes: `research/docs/native-graphics-notes.pt-BR.md`, "Atualização 3" and "Atualização 4".

## 3. Native renderer: ASTC, array/3D/cube textures and mipmaps
Text and some images use ASTC, which desktop GPUs do not sample; decode with suyu's `video_core/textures/astc.h`
before upload. Array (TIC type 5), 3D and cube textures and mip levels are not uploaded yet.

## 4. Native renderer: hybrid mode and `--native-graphics`
Draw each frame with our renderer when everything in it is supported, otherwise with the emulated GPU; ship it as an
experimental `play.sh --native-graphics` / `play.bat --native-graphics`. Move the NVN offsets (bootstrap
`nnSdk:0x502d10`, format table `0xb69208`, SSBO table `0x915550`) into `adapters/tomodachi/target.json`.

## 5. Lean runtime
Build a runtime with only what this game uses (no Qt GUI, no JIT, unused services and engines removed), so the game
runs on a small runtime of its own instead of the whole suyu.
