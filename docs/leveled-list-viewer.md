# Leveled List Viewer Instructions

* NEVER implement a fallback solution to read nif files from anywhere else than the `meshes` folder in the skyrim installation
* Always ignore character casing when looking up files on the file system
* How to determine which body slots are used by some outfit: Make a list of slots defined by all ARMO records. Ignore both the slots defined in the ARMA records and the NIF files
* When to add standard meshes for feet, hans & body: ONLY if the respective slots are not used by ANY ARMO record of the outfit currently under display
