# Specific instructions to all AI agents

* ALWAYS make sure to follow the physics simulation as done in FSMP (https://github.com/DaymareOn/hdtSMP64) to ensure that you'll also simulate possible errors in the physics configuration (xml) correctly
* NEVER introduce additional shapes or configurations for the physics simulation; just use what is there already
* NEVER implement workarounds or silent fallbacks in cases where the same thing would not work in-game either

## Keeping FSMP updates cheap

The SMP preview runs FSMP's own sources from `lib/hdtSMP64`. Updating FSMP must stay a pull + rebuild:

* FSMP's files are compiled **unmodified** (list in `src/fsmp/fsmp.cmake`: `hdtSkinnedMesh/*`, `ActorManager.cpp`, `hdtSkyrimSystem.cpp`, `hdtSkyrimPhysicsWorld.cpp`, `hdtSkyrimBone.cpp`, `hdtSkyrimBody.cpp`, `hdtDefaultBBP.cpp`, `config.cpp`, `Events.cpp`, `XmlReader.cpp`, ...), with FSMP's own `PCH.h` force-included. Do not port or copy them into `src/`.
* Not compiled: the game integration (`main.cpp`, `Hooks.cpp`, Papyrus and save-game code). `src/fsmp/StandIns.cpp` defines the few symbols of it that compiled files use (`OverrideManager`, whose overrides only Papyrus scripts and save games can set).
* Everything the standalone build needs from the game lives in fork-owned files under `src/fsmp/`:
  * `include/` — stand-in CommonLibSSE / SKSE / spdlog / Detours headers with the names and members FSMP uses. The scene graph types behave like the engine's: reference counting, world transform updates, the case-insensitive string pool, empty child slots after detaching, cloning.
  * `include-noppl/` (compilers without PPL) and `include-nowin/` (platforms other than Windows) — `ppl.h`, `d3d11.h`, `intrin.h` with the Win32 declarations FSMP uses. The PPL stand-in runs everything on the calling thread; `task_group::run` defers to `wait()` like PPL does.
  * `StandIns.cpp` — the stand-ins' behavior. Engine functions that can't exist outside the game (hooks, SKSE interfaces) log an error when called.
  * `FsmpScene.*` — the engine around FSMP: NIF → engine objects (`LoadModel`), equipping (`AttachArmor`: armor attach event, skin bound to the skeleton by bone name, attach event again), the FaceGen head (`AttachHead`: head skinning events), the inventory of equipped items (`SetWornItems`), frames (`Step`: frame event + frame sync event) and FSMP's plugin load (`Initialise`). The engine functions `ActorManager.cpp` calls through `REL::Relocation` (line of sight, FaceGen path, `NiStream`) are registered there.
* Bullet: `lib/bullet3` is checked out at the version of FSMP's vcpkg port (`cmake/ports/bullet3/vcpkg.json`). `fsmp.cmake` copies its sources into the build folder and runs the patch block of FSMP's `portfile.cmake` on them, so changes to FSMP's patch apply by themselves. The port's replacements are text searches; one that matches nothing leaves Bullet unchanged in FSMP's build as well, and CMake reports it (currently the non-Hookean spring calculation).
* After pulling FSMP: check out the Bullet version of its port in `lib/bullet3`, add new FSMP source files to `fsmp.cmake`, rebuild. Compile errors name the engine types or members a new FSMP version uses; add them to the stand-ins.
* If an FSMP file can't be used as-is, extend the stand-ins instead of editing the FSMP file. When an edit is unavoidable, keep it minimal and make it suitable for an upstream pull request. (The fork's older `HDT_SMP64_STANDALONE` compat layer in `lib/hdtSMP64` is not used: the build doesn't define the macro.)
