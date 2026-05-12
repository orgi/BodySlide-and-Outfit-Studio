# Leveled List Viewer Instructions

* NEVER implement a fallback solution to read nif files from anywhere else than the `meshes` folder in the skyrim installation
* Always ignore character casing when looking up files on the file system
* How to determine which body slots are used by some outfit: Make a list of slots defined by all ARMO records. Ignore both the slots defined in the ARMA records and the NIF files
* When to add standard meshes for feet, hans & body: ONLY if the respective slots are not used by ANY ARMO record of the outfit currently under display
* Body skin (NIFs and textures for slot 32 / 33 / 37) must follow the in-game resolution chain: NPC's WNAM → race's WNAM. No vanilla fallback when an NPC is selected. If neither NPC nor race resolves a slot's NIF (or textures), the slot is left unloaded / untextured and the failure is signaled both in the status bar and in the log.
* Race body / hair visibility (slots 31 hair, 32 body, 33 hands, 37 feet) at runtime is driven by what is **currently shown**, not by what the outfit declares. Implemented in `ApplyRaceBodyPartVisibility()` and re-run after every outfit-shape checkbox toggle so deselecting a covering outfit shape re-reveals the underlying race body part (and re-selecting it hides it again).
