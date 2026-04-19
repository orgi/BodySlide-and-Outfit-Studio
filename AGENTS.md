# General Instructions to AI agents

* For building & testing purposes, I'll run BodySlide using the following script: ./run-bodyslide.sh. When building please make sure to use the Release build folder
* EVERY error has to be logged in the log file.
* When using libraries, always prefer methods from the c++ std library over WxWidgets. Do NEVER use linux/posix libraries which will not work on Windows
* ALWAYS make sure that the implementation will be compatible with Linux and Windows
* For paths to the local Skyrim installation ALWAYS use skyrim-paths.md
* When updating files belonging to the original BodySlide or OutfitStudio code, make sure to ALWAYS follow the coding style you find and to ALWAYS keep the changes to an absolute minimum to make it easier to integrate those changes back upstream


## Documentation per Module
* The details of the modularizer are described in [`docs/modularizer.md`](docs/modularizer.md)
* The details of the leveled list viewer are described in [`docs/leveled-list-viewer.md`](docs/leveled-list-viewer.md)
