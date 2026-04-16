# General Instructions to AI agents

* For building & testing purposes, I'll run BodySlide using the following script: ./run-bodyslide.sh. When building please make sure to use the Release build folder
* EVERY error has to be logged in the log file.
* When using libraries, always prefer methods from the c++ std library over WxWidgets. Do NEVER use linux/posix libraries which will not work on Windows
* ALWYAS make sure that the implementation will be compatible with Linux and Windows

* For paths to the local Skyrim installation ALWAYS use skyrim-paths.md
* For the leveled list previewer:
  * NEVER implement a fallback solution to read nif files from anywhere else than the `meshes` folder in the skyrim installation
  * Always ignore character casing when looking up files on the file system
  * How to determine which body slots are used by some outfit: Make a list of slots defined by all ARMO records. Ignore both the slots defined in the ARMA records and the NIF files
  * When to add standard meshes for feet, hans & body: ONLY if the respective slots are not used by ANY ARMO record of the outfit currently under display