# Leveled List Viewer Instructions

* NEVER implement a fallback solution to read nif files from anywhere else than the `meshes` folder in the skyrim installation
* Always ignore character casing when looking up files on the file system
* How to determine which body slots are used by some outfit: Make a list of slots defined by all ARMO records. Ignore both the slots defined in the ARMA records and the NIF files
* When to add standard meshes for feet, hans & body: ONLY if the respective slots are not used by ANY ARMO record of the outfit currently under display
* Body skin (NIFs and textures for slot 32 / 33 / 37) must follow the in-game resolution chain: NPC's WNAM → race's WNAM. No vanilla fallback when an NPC is selected. If neither NPC nor race resolves a slot's NIF (or textures), the slot is left unloaded / untextured and the failure is signaled both in the status bar and in the log.
* Race body / hair visibility (slots 31 hair, 32 body, 33 hands, 37 feet) at runtime is driven by what is **currently shown**, not by what the outfit declares. Implemented in `ApplyRaceBodyPartVisibility()` and re-run after every outfit-shape checkbox toggle so deselecting a covering outfit shape re-reveals the underlying race body part (and re-selecting it hides it again).

## View modes

The left-hand pane is a `wxNotebook` with two tabs:

* **Outfits** — leveled-list / outfit browser as before (search + level filters + outfit list).
* **NPCs** — NPC browser. Selecting a row loads the NPC's head + race body + skin textures and, if available, automatically renders the NPC's **default outfit** (NPC_ `DOFT` → `OTFT`).

The currently selected NPC (`currentHeadEditorId`) is preserved across tab switches; switching tabs never clears the head or body. The `Head` text field at the top of the panel remains the shared selector and is updated whenever an NPC is picked in either tab.

## Default outfit (DOFT) resolution

* `NPCRecord.defaultOutfitFormId` is parsed from the `DOFT` subrecord and remapped from the source plugin's master space into the loaded ESP's `espMasters` space (last-writer-wins across overrides).
* `LeveledListData::ResolveNpcDefaultOutfit(npc)` looks up the `OTFT` in `otftCache` and builds an `OutfitEntry` on demand, caching it in `npcOutfitCache` keyed by editor id.
* `OTFT` `INAM` entries pointing at `LVLI` are expanded via `ResolveLVLIGrouped` and treated as **Use Any** groups. The previewer's default `UseAnyGroup::activeIndex = 0` naturally selects the **first valid variant**, matching the requirement.
* NPCs whose DOFT points at an OTFT or LVLI defined in a plugin **not** in the loaded ESP's master chain currently resolve to nullptr; the previewer logs the unresolved case and leaves the NPC standing in race body + head only.

## "Load all NPCs" button

By default only vanilla NPCs and overrides reachable from the loaded ESP's master chain are listed. The **Load all NPCs** button scans every non-vanilla plugin in the data directory for *native* `NPC_` records (top-byte == self) and appends them via `LeveledListData::LoadAllPluginNativeNPCs()`. After the scan the autocomplete map and the NPCs list are rebuilt.
