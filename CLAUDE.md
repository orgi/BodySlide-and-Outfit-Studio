# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

make sure to follow the instructions from @AGENTS.md

NEVER report a task as complete or claim a build is successful without first running the project's build script (./run-bodyslide.sh) and confirming it passes. ALWAYS include the build output in your verification step.

NEVER reference or hard-code any local file path. Neither in the code nor in any comment. NEVER share any information about the local development machine, installed mods or any other installed software or configuration.

## Build & run

* `./run-bodyslide.sh` — canonical build: configures CMake into `Release/` (`-DCMAKE_BUILD_TYPE=Release`), builds all targets, copies `res/` and the `OutfitStudio` binary into the BodySlide data dir, then launches `Release/BodySlide`.
* `./run-llview.sh` — same build, then launches the standalone `Release/LeveledListViewer`.
* Build only (no launch): `cmake --build Release -j` (or `make -C Release <target>` for one of `BodySlide`, `OutfitStudio`, `LeveledListViewer`).
* Don't use the `build/` or `build-mingw/` folders for verification; use `Release/`.
* There is no unit test suite. Verification means building and running the app, then checking the log file (see AGENTS.md → Logging; the install location is in `skyrim-paths.md`).
* Dependencies (see `Readme-linux.txt`): wxWidgets ≥ 3.1.3, GLEW, OpenGL, FBX SDK (OutfitStudio only, at `../fbxsdk` relative to the repo). Bullet is built from `lib/bullet3` with FSMP's patch (`src/fsmp/fsmp.cmake`, needs CMake ≥ 3.25), not taken from the system. At runtime the executables need `res/` (XRC layouts, icons) next to them or reachable via `WX_BODYSLIDE_DATA_DIR` / `WX_OUTFITSTUDIO_DATA_DIR`.

## Architecture

Three executables are built from one shared source set (`commonsources` in `CMakeLists.txt`): nifly (NIF I/O, git submodule in `lib/nifly`), FSEngine (BSA/BA2), the slider/morph data model, the OpenGL renderer and ESP read/write.

* **BodySlide** (`src/program/BodySlideApp.*`) — builds outfit/body meshes from slider presets. It also hosts the Leveled List Previewer (`ShowLeveledListPreviewer()`) and the `PreviewWindow`.
* **OutfitStudio** (`src/program/OutfitStudio.*`, `OutfitProject.*`) — mesh/slider editor. The **Modularizer** (`OnModularizeShapes`) lives here; spec in `docs/modularizer.md`.
* **LeveledListViewer** (`src/program/LeveledListApp.*`) — a standalone wrapper around the same `LeveledListPreviewer` that BodySlide uses. Changes to `LeveledListPreviewer.cpp` / `LeveledListData.cpp` affect both binaries. Spec and invariants in `docs/leveled-list-viewer.md`.

Fork-specific subsystems (not in upstream BodySlide):

* `src/components/LeveledListData.*` — plugin/record layer on top of `src/files/ESPReader.*`. It loads the active load order from `Plugins.txt`, caches RACE/ARMO/ARMA/TXST/OTFT/LVLI/NPC_ records, remaps FormIDs across master spaces and resolves NPC default outfits and skin chains.
* `src/program/LeveledListPreviewer.*` — the wx UI with Outfits/NPCs tabs. It renders NPC head (FaceGen NIF + preset morphs from `.tri`), race body parts, skin textures and outfit pieces through `GLSurface`.
* `src/fsmp/` — FSMP (`lib/hdtSMP64`, compiled unmodified, static library `fsmp` built as C++23) with the patched Bullet (`fsmp_bullet`), stand-in game headers and `FsmpScene`, the engine emulation around FSMP. Read `src/components/agents.md` before touching it (it also describes how FSMP updates are kept cheap). `FsmpScene.h` is the only header the rest of the code includes; it is free of FSMP and Bullet.
* `src/components/ActorAnimation.*` + `src/files/HkxFile.*` — the previewer's actor in motion: Havok packfile reader (skeleton, spline-compressed clips, behavior project / character strings), the chosen animation (default: the idle) posing the FSMP scene's skeleton, FSMP physics and CPU skinning of the displayed meshes. Rules in `docs/leveled-list-viewer.md` → Animation and SMP physics.

Other layers: `src/render/` (GL surface, shaders, materials), `src/files/` (TRI, OBJ, FBX, DDS, material, ESP formats), `src/utils/` (logging via `Log.*`, config, platform helpers), `src/ui/` (custom wx widgets), `res/xrc/` (dialog layouts loaded at runtime, not compiled in).

## Conventions & gotchas

* Code style is in `.clang-format` (tabs, `{` on the same line). When touching upstream files, match the local style exactly and keep diffs minimal (see AGENTS.md).
* New source files must be added to the right source list in `CMakeLists.txt`. The Windows `.vcxproj`/`.filters` files are maintained separately: `LeveledListViewer.vcxproj` contains the fork's components but `BodySlide.vcxproj` currently does not, so Windows builds of BodySlide need those files added. Neither project builds FSMP and its patched Bullet (`src/fsmp/fsmp.cmake`); use CMake for that.
* Game file lookups must be case-insensitive. Linux filesystems are case-sensitive, but game data and plugins assume Windows semantics.
* A PostToolUse hook (`.claude/hooks/check-local-refs.sh`) rejects edits that add absolute local paths or non-vanilla plugin names to source/doc files.
* Each module's behavioral rules live in `docs/*.md`. When the user asks for a new feature or a strategy change, update the matching doc.
