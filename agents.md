# General Instructions to AI agents

* For testing purposes, I'll run BodySlide using the following script: ./run-bodyslide.sh. When building please make sure to use the Release build folder
* EVERY error has to be logged in the log file.
* When using libraries, always prefer methods from the c++ std library over WxWidgets. Do NEVER use linux/posix libraries which will not work on Windows
* ALWYAS make sure that the implementation will be compatible with Linux and Windows
* NEVER work around bugs in .esp files loaded in the leveled list previewer (except for upper/lowercase path mismatches).
* For paths to the local Skyrim installation ALWAYS use skyrim-paths.md
